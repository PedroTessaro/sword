# Writing an HTTP server

`std/http` is a small HTTP/1.1 server and client, written in Sword on top of
`std/net`. It supports keep-alive; it does not do TLS or chunked transfer
encoding.

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

This shape is forced by two things about Sword, and they turn out to fit well
together. A task cannot outlive its `scope`, so "one task per connection,
started whenever a connection arrives" has nowhere to put the scope. And I/O is
blocking, so a task waiting on a socket holds its worker either way. A fixed
pool of loops inside one long-lived scope answers both.

`ServeWith(h, n)` picks the number of loops; `Serve` uses four.

**Each loop has its own arena.** That is not an optimisation, it is a
requirement the compiler enforces: an allocator is mutable state, and handing
the same one to two tasks is a compile error. Every loop builds a 256 KiB arena
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
carry a `Content-Length`. There is no routing beyond what your handler writes
with `if`, no cookie or form parsing, and no timeouts: a client that opens a
connection and says nothing occupies an accept loop until it goes away.
