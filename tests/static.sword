// expect: 42
// expect-output: files served
// Serving a file is streamed against a length the operating system already knows,
// so a large download costs one buffer. And the part that matters more than
// speed: a path that tries to climb out of the root is refused, not resolved.

import "std/fs"
import "std/http"
import "std/io"
import "std/net"
import "std/strings"

func serve(s *http.Server, m *http.Mux) !void {
    try s.ServeWith(m, 32)
}

// Sends one request and returns everything that came back.
func ask(port i32, request string, mut into []u8) !u64 {
    mut c := try net.Dial("127.0.0.1", port)
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
    mut room := [8192]u8{}

    n := ask(port, "GET /files/hello.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             room[..]) catch 0
    reply := string(room[0..n])
    if strings.Contains(reply, "200 OK") &&
       strings.Contains(reply, "Content-Length: 11") &&
       strings.Contains(reply, "text/plain") &&
       strings.Contains(reply, "hello world") {
        score.Add(1)
    }

    // The index, for a path that names the directory.
    m := ask(port, "GET /files/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             room[..]) catch 0
    index := string(room[0..m])
    if strings.Contains(index, "200 OK") &&
       strings.Contains(index, "text/html") &&
       strings.Contains(index, "<h1>index</h1>") {
        score.Add(2)
    }

    // Out of the root: refused on sight.
    b := ask(port, "GET /files/../../etc/passwd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             room[..]) catch 0
    climb := string(room[0..b])
    if strings.Contains(climb, "400") && !strings.Contains(climb, "root:") {
        score.Add(4)
    }

    // Nothing there.
    g := ask(port, "GET /files/nope.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             room[..]) catch 0
    if strings.Contains(string(room[0..g]), "404") {
        score.Add(8)
    }

    // HEAD: the head of the answer and none of the body.
    h := ask(port, "HEAD /files/hello.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
             room[..]) catch 0
    head := string(room[0..h])
    if strings.Contains(head, "Content-Length: 11") &&
       !strings.Contains(head, "hello world") {
        score.Add(16)
    }

    // A file larger than one read: the pieces have to arrive in order and all of
    // them, which a length in the head makes checkable.
    big := ask(port, "GET /files/big.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
               room[..]) catch 0
    whole := string(room[0..big])
    if strings.Contains(whole, "Content-Length: 5000") &&
       strings.Contains(whole, "application/octet-stream") &&
       big >= 5000 {
        score.Add(32)
    }

    s.Close()
}

func main() !int {
    // One directory, three files in it.
    try fs.MakeDir("/tmp/sword_static")
    try fs.WriteAll("/tmp/sword_static/hello.txt", []u8("hello world"))
    try fs.WriteAll("/tmp/sword_static/index.html", []u8("<h1>index</h1>\n"))

    mut big := [5000]u8{}
    for i in 0..5000 {
        big[i] = u8(48 + i % 10)
    }
    try fs.WriteAll("/tmp/sword_static/big.bin", big[..])

    mut files := http.NewFiles("/tmp/sword_static")

    mut mux := http.NewMux()
    try mux.Get("/files/{path...}", &files)
    try mux.Handle("HEAD", "/files/{path...}", &files)

    mut srv := try http.Listen(0)
    port := srv.Port()

    mut score := atomic[u64](0)
    scope {
        spawn serve(&srv, &mux)
        spawn drive(port, &score, &srv)
    }

    fs.Remove("/tmp/sword_static/hello.txt") catch {}
    fs.Remove("/tmp/sword_static/index.html") catch {}
    fs.Remove("/tmp/sword_static/big.bin") catch {}
    fs.RemoveDir("/tmp/sword_static") catch {}

    if score.Load() != 63 {
        try io.Printf("score {}\n", score.Load())
        return 1
    }
    try io.Print("files served\n")
    return 42
}
