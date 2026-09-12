package http

import "std/strings"

const MaxRoutes = 64

struct route {
    method  string
    pattern string
    handler Handler
}

// Sends a request to one of several handlers by method and path, and is a
// Handler itself, so it goes straight to Server.Serve:
//
//     mut mux := http.NewMux()
//     try mux.Get("/users/{id}", &users)
//     try mux.Post("/users", &create)
//     try srv.Serve(&mux)
//
// A `{name}` in a pattern matches one whole path segment and leaves it behind
// for req.Param("name"). Routes are tried in the order they were added.
struct Mux {
    routes [MaxRoutes]route
    count  u64
}

func NewMux() Mux {
    return Mux{routes: [MaxRoutes]route{}, count: 0}
}

func (mut m *Mux) Handle(method string, pattern string, h Handler) !void {
    if m.count >= MaxRoutes {
        return error.TooManyRoutes
    }
    m.routes[m.count] = route{method: method, pattern: pattern, handler: h}
    m.count += 1
}

func (mut m *Mux) Get(pattern string, h Handler) !void {
    try m.Handle("GET", pattern, h)
}

func (mut m *Mux) Post(pattern string, h Handler) !void {
    try m.Handle("POST", pattern, h)
}

func (mut m *Mux) Put(pattern string, h Handler) !void {
    try m.Handle("PUT", pattern, h)
}

func (mut m *Mux) Delete(pattern string, h Handler) !void {
    try m.Handle("DELETE", pattern, h)
}

// One path segment and what follows it, the same shape the header parser uses
// for lines.
struct segment {
    text string
    rest string
}

func nextSegment(s string) segment {
    cut := strings.IndexByte(s, 47) // '/'
    if cut == s.len {
        return segment{text: s, rest: s[s.len..s.len]}
    }
    return segment{text: s[0..cut], rest: s[cut+1..s.len]}
}

// `/users/` and `/users` are the same route, so the slashes on either end are
// not part of the comparison.
func trimSlashes(s string) string {
    mut start u64 = 0
    if s.len > 0 && s[0] == 47 {
        start = 1
    }
    mut stop := s.len
    for stop > start && s[stop-1] == 47 {
        stop -= 1
    }
    return s[start..stop]
}

func isWildcard(s string) bool {
    return s.len >= 2 && s[0] == 123 && s[s.len-1] == 125 // {name}
}

// Walks both sides a segment at a time, recording what the wildcards caught.
// `req` is the copy the mux is about to hand to the handler, never the one it
// was given.
func matchPattern(pattern string, path string, mut req *Request) bool {
    mut want := trimSlashes(pattern)
    mut have := trimSlashes(path)
    req.pcount = 0
    for {
        if want.len == 0 || have.len == 0 {
            return want.len == 0 && have.len == 0
        }
        left := nextSegment(want)
        right := nextSegment(have)
        want = left.rest
        have = right.rest

        if isWildcard(left.text) {
            if req.pcount < MaxParams {
                req.params[req.pcount] = Param{
                    Name:  left.text[1..left.text.len-1],
                    Value: right.text,
                }
                req.pcount += 1
            }
        } else if !strings.Equal(left.text, right.text) {
            return false
        }
    }
}

// A path that matched under a different method is a 405, not a 404: the
// difference tells a client whether retrying with another verb is worth it.
func (m *Mux) Serve(req *Request, mut res *Response) !void {
    mut routed := *req
    mut wrongMethod := false

    for i in 0..m.count {
        if !matchPattern(m.routes[i].pattern, req.Path, &routed) {
            continue
        }
        if !strings.Equal(m.routes[i].method, req.Method) {
            wrongMethod = true
            continue
        }
        try m.routes[i].handler.Serve(&routed, res)
        return
    }

    if wrongMethod {
        try res.Text(405, "method not allowed\n")
        return
    }
    try res.Text(404, "not found\n")
}
