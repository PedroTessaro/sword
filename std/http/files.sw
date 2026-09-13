package http

import "std/fs"
import "std/strings"

error NoSuchFile = "there is no file at that path"
error ShortFile  = "the file is smaller than it said it was"

// How much of a file goes out at a time. Big enough that a megabyte is a few
// dozen writes, small enough to sit on a task's stack beside everything else a
// connection needs.
const FileChunk = 32768

// Longest path this will build. A request that would need more is refused rather
// than truncated, because a truncated path names a different file.
const MaxPath = 512

// What to serve when the path names a directory.
const DefaultIndex = "index.html"

// Enough of the table to cover what a server actually sends. Anything unknown
// goes out as a stream of bytes, which browsers will offer to save rather than
// guess at.
func MimeType(path string) string {
    cut := strings.LastIndexByte(path, 46) // '.'
    if cut == path.len {
        return "application/octet-stream"
    }
    ext := path[cut+1..path.len]
    if strings.EqualFold(ext, "html") || strings.EqualFold(ext, "htm") {
        return "text/html; charset=utf-8"
    }
    if strings.EqualFold(ext, "css") {
        return "text/css; charset=utf-8"
    }
    if strings.EqualFold(ext, "js") || strings.EqualFold(ext, "mjs") {
        return "text/javascript; charset=utf-8"
    }
    if strings.EqualFold(ext, "json") {
        return "application/json"
    }
    if strings.EqualFold(ext, "txt") || strings.EqualFold(ext, "md") {
        return "text/plain; charset=utf-8"
    }
    if strings.EqualFold(ext, "svg") {
        return "image/svg+xml"
    }
    if strings.EqualFold(ext, "png") {
        return "image/png"
    }
    if strings.EqualFold(ext, "jpg") || strings.EqualFold(ext, "jpeg") {
        return "image/jpeg"
    }
    if strings.EqualFold(ext, "gif") {
        return "image/gif"
    }
    if strings.EqualFold(ext, "webp") {
        return "image/webp"
    }
    if strings.EqualFold(ext, "ico") {
        return "image/x-icon"
    }
    if strings.EqualFold(ext, "woff2") {
        return "font/woff2"
    }
    if strings.EqualFold(ext, "wasm") {
        return "application/wasm"
    }
    if strings.EqualFold(ext, "pdf") {
        return "application/pdf"
    }
    if strings.EqualFold(ext, "mp4") {
        return "video/mp4"
    }
    return "application/octet-stream"
}

// Sends a file as the body, a piece at a time. A hundred-megabyte download costs
// one buffer rather than a hundred megabytes, and the length goes out in the head
// because the operating system already knows it.
//
//     func (h *Site) Serve(req *http.Request, mut res *http.Response) !void {
//         try http.ServeFile(res, "public/index.html")
//     }
//
// The Content-Type is guessed from the name unless the handler already set one.
// Reading a file puts the task down like any other wait; the thread it borrows
// comes from the pool the runtime keeps for work that cannot be polled.
func ServeFile(mut res *Response, path string) !void {
    known := fs.Size(path)
    if known == nil {
        return error.NoSuchFile
    }
    try sendFile(res, path, known orelse 0)
}

// The head, then the file in pieces. Split out because a HEAD request needs the
// first half of this and none of the second.
func sendFile(mut res *Response, path string, size u64) !void {
    mut f := try fs.Open(path, fs.Mode.Read)
    defer f.Close()

    if res.Headers.Get("Content-Type").len == 0 {
        try res.SetHeader("Content-Type", MimeType(path))
    }
    try res.Send(200, size)

    mut buf := [FileChunk]u8{}
    mut left := size
    for left > 0 {
        mut want := left
        if want > FileChunk {
            want = FileChunk
        }
        n := try f.Read(buf[0..want])
        if n == 0 {
            // Shorter than it said it was, so somebody is writing to it. The
            // client is already waiting on a length that will not arrive.
            return error.ShortFile
        }
        try res.Write(buf[0..n])
        left -= n
    }
}

// Serves a directory of files, and is a Handler, so it goes straight into a
// route:
//
//     mut site := http.NewFiles("public")
//     try mux.Get("/static/{path...}", &site)
//
// The part the `{path...}` caught is what gets looked up, so the route decides
// what the URL looks like and this decides what is on disk. A path with a `..`
// in it is refused rather than resolved: resolving is where traversal bugs live.
struct Files {
    Root string
    // Served when the path names a directory. Empty turns directories into 404s.
    Index string
    // Which parameter holds the path. Empty means the whole request path.
    Param string
}

func NewFiles(root string) Files {
    return Files{Root: root, Index: DefaultIndex, Param: "path"}
}

// No `..` anywhere, no empty segments, no backslashes, nothing but a path that
// stays underneath. A dot on its own is fine — `.hidden` is a name, not a climb.
func safeRelative(path string) bool {
    if path.len == 0 {
        return true
    }
    mut at u64 = 0
    mut segment u64 = 0
    for at < path.len {
        c := path[at]
        if c == 0 || c == 92 { // NUL, backslash
            return false
        }
        if c == 47 { // '/'
            if segment == 0 {
                return false // '//' or a leading slash after trimming
            }
            segment = 0
        } else {
            segment += 1
        }
        at += 1
    }
    // Walk the segments again for `..`, which is the one that matters.
    mut rest := path
    for {
        cut := strings.IndexByte(rest, 47)
        part := rest[0..cut]
        if part == ".." {
            return false
        }
        if cut == rest.len {
            return true
        }
        rest = rest[cut+1..rest.len]
    }
}

func (f *Files) Serve(req *Request, mut res *Response) !void {
    if req.Method != "GET" && req.Method != "HEAD" {
        try res.Text(405, "method not allowed\n")
        return
    }

    mut rel := req.Path
    if f.Param.len > 0 {
        rel = req.Param(f.Param)
    }
    // Leading slashes are the route's, not the file's.
    for rel.len > 0 && rel[0] == 47 {
        rel = rel[1..rel.len]
    }

    directory := rel.len == 0 || rel[rel.len-1] == 47
    if directory && f.Index.len == 0 {
        try res.Text(404, "not found\n")
        return
    }

    if !safeRelative(rel) {
        try res.Text(400, "bad path\n")
        return
    }

    mut index := ""
    if directory {
        index = f.Index
    }
    mut room := [MaxPath]u8{}
    n := joinPath(room[..], f.Root, rel, index)
    if n == 0 {
        try res.Text(414, "path too long\n")
        return
    }
    path := string(room[0..n])

    known := fs.Size(path)
    if known == nil {
        try res.Text(404, "not found\n")
        return
    }
    size := known orelse 0

    // A HEAD answer is the head of the GET answer: same length, same type, no
    // body. The client asked what is there, not for it.
    if req.Method == "HEAD" {
        try res.SetHeader("Content-Type", MimeType(path))
        try res.Send(200, size)
        return
    }

    sendFile(res, path, size) catch {
        // Only reachable before the head went out; after that there is nothing
        // left to say on this connection.
        if !res.sent {
            try res.Text(404, "not found\n")
        }
    }
}

// `root/rel` plus an index name when the path names a directory, into the
// caller's buffer. Zero when it would not fit.
func joinPath(mut into []u8, root string, rel string, index string) u64 {
    mut at u64 = 0
    if root.len + rel.len + index.len + 2 > into.len {
        return 0
    }
    for c in root {
        into[at] = c
        at += 1
    }
    if at > 0 && into[at-1] != 47 {
        into[at] = 47
        at += 1
    }
    for c in rel {
        into[at] = c
        at += 1
    }
    for c in index {
        into[at] = c
        at += 1
    }
    return at
}
