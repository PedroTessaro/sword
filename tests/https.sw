// expect: 42
// expect-output: https checked
// The same server, the same handler, the same router — over TLS. The only line
// that knows is the one that listens, which is the point of the server reading a
// stream rather than a socket.

import "std/fs"
import "std/http"
import "std/io"
import "std/mem"
import "std/net"
import "std/strings"
import "std/tls"

const certPath = "/tmp/sword_https_cert.pem"
const keyPath = "/tmp/sword_https_key.pem"

struct Site {
    n u64
}

func (h *Site) Serve(req *http.Request, mut res *http.Response) !void {
    if req.Path == "/stream" {
        try res.Stream(200)
        for i in 0..3 {
            try res.Printf("part{} ", i)
        }
        return
    }
    try res.Printf("hello {} from {}\n", req.Param("name"), req.RemoteAddr)
}

func serve(s *http.Server, m *http.Mux) !void {
    try s.ServeWith(m, 32)
}

// A request by hand over TLS, because the client in std/http speaks plain HTTP.
func ask(ctx *tls.Context, port i32, request string, mut into []u8) !u64 {
    mut c := try tls.Dial(ctx, "127.0.0.1", port)
    try c.WriteString(request)
    mut have u64 = 0
    for {
        n := c.Read(into[have..into.len]) catch 0
        if n == 0 {
            break
        }
        have += n
    }
    c.Close()
    return have
}

func drive(port i32, mut score *atomic[u64], s *http.Server) !void {
    mut conf := tls.NewConfig()
    conf.CAFile = certPath // the certificate is its own authority here
    mut ctx := tls.ClientContext(conf) catch {
        s.Close()
        return
    }

    mut room := [4096]u8{}
    n := ask(&ctx, port, "GET /hello/world HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             room[..]) catch 0
    reply := string(room[0..n])
    if strings.Contains(reply, "200 OK") &&
       strings.Contains(reply, "hello world from 127.0.0.1") {
        score.Add(1)
    }

    // Chunked over TLS: two framings, one on top of the other.
    m := ask(&ctx, port, "GET /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             room[..]) catch 0
    // Each Printf is its own chunk, so the parts arrive framed apart rather than
    // as one string.
    streamed := string(room[0..m])
    if strings.Contains(streamed, "Transfer-Encoding: chunked") &&
       strings.Contains(streamed, "part0 ") &&
       strings.Contains(streamed, "part2 ") &&
       strings.Contains(streamed, "0\r\n\r\n") {
        score.Add(2)
    }

    // Keep-alive over TLS: two requests, one connection, one handshake.
    mut c := tls.Dial(&ctx, "127.0.0.1", port) catch {
        ctx.Free()
        s.Close()
        return
    }
    mut ok u64 = 0
    for i in 0..2 {
        c.WriteString("GET /hello/again HTTP/1.1\r\nHost: x\r\n\r\n") catch break
        mut back := [1024]u8{}
        got := c.Read(back[..]) catch 0
        if strings.Contains(string(back[0..got]), "hello again") {
            ok += 1
        }
    }
    c.Close()
    if ok == 2 {
        score.Add(4)
    }

    // The client in std/http, over TLS, against this server.
    mut backing := [131072]u8{}
    mut arena := mem.NewArena(backing[..])
    mut client := http.NewClient()
    client.TLS.CAFile = certPath
    fetched := client.GetTLS("127.0.0.1", port, "/hello/client", &arena) catch {
        ctx.Free()
        s.Close()
        return
    }
    if fetched.Status == 200 &&
       strings.Contains(string(fetched.Body), "hello client") {
        score.Add(8)
    }

    // And a client that will not trust it gets nowhere, which is the same
    // certificate and a different answer.
    mut strict := tls.NewConfig()
    mut plain := tls.ClientContext(strict) catch {
        ctx.Free()
        s.Close()
        return
    }
    refused := tls.Dial(&plain, "127.0.0.1", port) catch {
        score.Add(16)
        plain.Free()
        ctx.Free()
        s.Close()
        return
    }
    mut open := refused
    open.Close()
    plain.Free()
    ctx.Free()
    s.Close()
}

func main() !int {
    if !tls.Available() {
        // Without OpenSSL there is nothing to serve over, and the answer has to
        // be an error rather than a plain-text server on a port nobody expects.
        mut refused := false
        http.ListenTLS(0, certPath, keyPath) catch {
            refused = true
        }
        if !refused {
            return 1
        }
        try io.Print("https checked: this build has no TLS\n")
        return 42
    }

    try tls.SelfSigned("127.0.0.1", certPath, keyPath)

    mut site := Site{n: 0}
    mut mux := http.NewMux()
    try mux.Get("/hello/{name}", &site)
    try mux.Get("/stream", &site)

    mut srv := try http.ListenTLS(0, certPath, keyPath)
    if !srv.Secure() {
        return 2
    }
    port := srv.Port()

    mut score := atomic[u64](0)
    scope {
        spawn serve(&srv, &mux)
        spawn drive(port, &score, &srv)
    }
    srv.Free()

    fs.Remove(certPath) catch {}
    fs.Remove(keyPath) catch {}

    if score.Load() != 31 {
        try io.Printf("score {}\n", score.Load())
        return 3
    }
    // Two of those were served on one connection, so more requests than
    // connections is the shape to expect.
    if srv.Served() < 5 || srv.Accepted() < 4 {
        return 4
    }
    try io.Print("https checked: plain, chunked, kept alive and refused\n")
    return 42
}
