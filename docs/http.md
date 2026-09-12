# Writing an HTTP server

`std/http` is a small HTTP/1.1 server and client, written in Sword on top of
`std/net`. It does keep-alive, routing with path parameters, query strings and
timeouts; it does not do TLS or chunked transfer encoding.

## A server

```sword
import "std/http"
import "std/strings"

struct Router {
    greeting string
}

func (r *Router) Serve(req *http.Request, mut res *http.Response) !void {
    if strings.Equal(req.Path, "/health") {
        try res.Text(200, "ok")
        return
    }
    if strings.Equal(req.Path, "/") {
        try res.Text(200, r.greeting)
        return
    }
    try res.Text(404, "not found")
}

func main() !int {
    mut server := try http.Listen(8080)
    mut router := Router{greeting: "hello\n"}
    try server.Serve(&router)
    return 0
}
```

A handler is an interface, not a function value, the same way Go's
`http.Handler` is. That gives the handler somewhere to keep its own state —
`Router` above carries a greeting, and it could just as well carry a database
handle or a counter.

Note the `&`. Only pointers satisfy an interface in Sword, because boxing a
value would mean allocating.

## How it runs

`Serve` opens a `scope` and spawns a fixed number of accept loops inside it.
Each one blocks in `accept`, handles the connection it gets, and goes back for
another:

```sword
scope {
    for i in 0..workers {
        spawn acceptLoop(s, h)
    }
}
```

The kernel spreads incoming connections across the loops. A slow request
occupies one of them, not the server.

This shape is forced by one thing about Sword: a task cannot outlive its
`scope`, so "one task per connection, started whenever a connection arrives"
has nowhere to put the scope. A fixed pool of loops inside one long-lived scope
answers that.

`ServeWith(h, n)` picks the number of loops; `Serve` uses 32. That number is
the number of connections the server can be in the middle of, and it can be far
larger than the core count: a task waiting on a socket tells the scheduler, and
the pool grows a thread to cover for it. [Concurrency](concurrency.md#blocking-io)
has the details. Raising it costs a thread and an arena per loop, nothing else.

**Each loop has its own arena.** That is not an optimisation, it is a
requirement the compiler enforces: an allocator is mutable state, and handing
the same one to two tasks is a compile error. Every loop builds a 64 KiB arena
on its own stack and resets it after each connection, so nothing is shared and
nothing leaks between requests.

## Stopping

`Close` shuts the listener down, which wakes every blocked `accept`. The loops
return, the scope joins, and `Serve` comes back:

```sword
scope {
    spawn runServer(&server, &router)
    spawn runUntilDone(&server)
}
```

A plain `close` would not be enough — on BSD it does not wake a thread already
inside `accept`, so `Close` shuts the socket down first.

## Routing

The `if` chain above gets old quickly. `Mux` is a handler that dispatches to
other handlers by method and path:

```sword
import "std/http"

struct Users {
    seen u64
}

func (h *Users) Serve(req *http.Request, mut res *http.Response) !void {
    try res.Printf("user {}\n", req.Param("id"))
}

struct Health {
    started i64
}

func (h *Health) Serve(req *http.Request, mut res *http.Response) !void {
    try res.Text(200, "ok\n")
}

func main() !int {
    mut users := Users{seen: 0}
    mut health := Health{started: 0}

    mut mux := http.NewMux()
    try mux.Get("/health", &health)
    try mux.Get("/users/{id}", &users)
    try mux.Post("/users", &users)

    mut server := try http.Listen(8080)
    try server.Serve(&mux)
    return 0
}
```

A `{name}` matches exactly one path segment and leaves what it matched behind
for `req.Param("name")`. So `/users/{id}` matches `/users/7` and not
`/users/7/posts`. Routes are tried in the order you added them, trailing
slashes are ignored, and there are `Get`, `Post`, `Put`, `Delete` and the
general `Handle(method, pattern, h)`.

Two answers come from the mux itself. A path no route matched is a 404. A path
that matched under a different method is a **405**, which is the more useful
reply: it tells the client that retrying with another verb might work.

A mux holds up to 64 routes in a fixed array — it never allocates, so it can
live on the stack of `main` and be shared by every accept loop.

## Reading a request

```sword
func (r *Router) Serve(req *http.Request, mut res *http.Response) !void {
    if !strings.Equal(req.Method, "POST") {
        try res.Text(405, "method not allowed")
        return
    }

    kind := req.Headers.Get("Content-Type")     // case-insensitive
    if !strings.HasPrefix(kind, "application/json") {
        try res.Text(415, "send me json")
        return
    }

    try res.JSON(200, "{\"length\":0}")
    try res.Write(req.Body)
}
```

`req.Path`, `req.Method` and the header strings all point into the connection's
read buffer. They are valid while the handler runs and not afterwards — copy
anything you mean to keep.

A request is capped at 32 headers and 16 KiB of head. Past that the connection
is dropped rather than grown, which keeps a hostile client from deciding how
much memory the server uses.

### The query string

`GET /search?q=ada+king&page=2` arrives split in three:

| Field | Value |
|---|---|
| `req.Target` | `/search?q=ada+king&page=2` |
| `req.Path` | `/search` |
| `req.RawQuery` | `q=ada+king&page=2` |

`req.Query("page")` gives `2`. It is still percent-encoded, because decoding
means writing somewhere and a request has no allocator — so decoding is a
separate step, with the space coming from you:

```sword
mut room := [64]u8{}
q := try http.Unescape(req.Query("q"), room[..])   // "ada king"
```

`http.Escape` goes the other way, for building a target out of a value that may
contain anything. Both fail with `error.NoSpace` rather than writing past the
end of what you gave them.

## The client

```sword
import "std/http"
import "std/mem"

func main() !int {
    mut backing := [65536]u8{}
    mut arena := mem.NewArena(backing[..])

    res := try http.Get("127.0.0.1", 8080, "/health", &arena)
    if res.Status != 200 {
        return 1
    }

    body := "{\"name\":\"sword\"}"
    posted := try http.Post("127.0.0.1", 8080, "/echo", "application/json",
                            []u8(body), &arena)
    return int(posted.Status / 100)
}
```

The response body lives in the allocator you pass in, so it stays valid until
you reset or free that allocator. The client sends `Connection: close` and does
not reuse sockets.

Both of those go through a default `Client`, which gives up after 10 seconds
trying to connect and 30 seconds waiting for a reply. Build your own when those
are wrong:

```sword
import "std/time"

mut client := http.NewClient()
client.Connect = time.Millis(500)
client.Read = time.Seconds(5)

res := try client.Get("127.0.0.1", 8080, "/health", &arena)
```

The two limits are separate because they fail for different reasons. A host
that is not listening answers within a moment, so a short connect timeout
catches a wrong address without cutting off a server that is merely thinking. A
zero duration on either waits as long as the kernel would — which for connect
is over a minute, and is why the default is not zero.

## Testing a server without leaving the program

Because a server is just a task, a test can run both sides at once:

```sword
scope {
    spawn serve(&server, &router)
    spawn drive(port, &server, results)
}
```

`drive` makes its requests, writes what it found into a slice the parent owns,
and then calls `server.Close()` — which is what lets the scope join and the
program finish. `tests/http.sw` and `tests/httpkeepalive.sw` are exactly this.

Both tasks are handed `&server`, and neither takes it as `mut`: serving and
closing only read the descriptor. Mark either parameter `mut` and the race
checker rejects the program, which is the correct answer to "two tasks are
writing the same thing".

## What is missing

No TLS, and no `Transfer-Encoding: chunked` — a request or response has to
carry a `Content-Length`, so a reply whose length is not known up front has to
be buffered before it is sent. There is no cookie or form parsing, and the
client does not follow redirects or keep connections alive between calls.
