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

One task per connection. The accept loop is the body of the scope rather than a
task in it, because only the body may spawn:

```sword
scope {
    for {
        c := s.listener.Accept() catch break
        spawn serveConn(c, h, s)
    }
}
```

That shape is only affordable because a task waiting on a socket costs a stack
and not a thread — the task is put down and the worker goes to whoever has data.
See [Concurrency](concurrency.md#waiting).

Measured on ten cores, with keep-alive, which is what every real client does:

| Connections at once | |
|---|---|
| 33 | 198 000 req/s |
| 1 000 | 146 000 req/s, 12 threads, 34 MiB |
| 4 000 | 84 000 req/s, 12 threads |

Three thousand idle keep-alive connections sit in 143 MiB on twelve threads —
about 49 KiB each.

**Each connection has its own arena**, 32 KiB on its own stack. That is not an
optimisation, it is a requirement the compiler enforces: an allocator is mutable
state, and handing the same one to two tasks is a compile error.

`ServeWith(h, n)` caps how many connections are served at once; `Serve` allows
1024. Past the cap the server stops accepting and the kernel's backlog holds the
rest, which is what backpressure should look like from outside: a queue, not a
collapse. `s.Live()` says how many are in flight.

## Stopping

`Close` shuts the listener down, which ends the accept loop. The scope then
waits for every connection still being served, so shutting down drains rather
than cuts:

```sword
scope {
    spawn runServer(&server, &router)
    spawn runUntilDone(&server)
}
```

A plain `close` would not be enough — on BSD it does not wake a thread already
inside `accept`, so `Close` shuts the socket down first.

## Limits

Two, and they do different jobs:

```sword
try c.SetTimeout(time.Seconds(15))                    // any one wait
try c.SetDeadline(time.Now().Add(time.Seconds(30)))   // the whole exchange
```

The server sets both: fifteen seconds for a single wait, and thirty for one turn
of the keep-alive loop — waiting for a request and serving it. A timeout alone
cannot bound a client that dribbles a byte at a time, because no single wait ever
runs out.

A request is capped at 32 headers and 16 KiB of head. Past that the connection is
dropped rather than grown, which keeps a hostile client from deciding how much
memory the server uses.

## Watching it run

A server nobody can see inside is a server nobody can run. Four counters on the
server, and the scheduler's own next to them:

```sword
import "std/runtime"

func (h *Admin) Serve(req *http.Request, mut res *http.Response) !void {
    r := runtime.Read()
    try res.Printf("threads {} stacks {} running {}\n",
                   r.Threads, r.Stacks, r.Running())
    res.Status = 200
}
```

`s.Live()` is connections in flight, `s.Accepted()` how many have been taken
altogether, and `s.Served()` and `s.Failed()` requests answered and handlers that
did not. `runtime.Queued` is the one to watch: a queue that keeps growing means
the server is taking more than it can serve.

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
