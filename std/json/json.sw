package json

import "std/bytes"
import "std/collections"
import "std/fmt"
import "std/mem"

// The document is one flat list of nodes, and a node points at its children by
// index rather than by pointer. That keeps everything in a single allocation
// and sidesteps the recursive type a tree would otherwise need.

const Null = 0
const Bool = 1
const Number = 2
const Str = 3
const Array = 4
const Object = 5

const noNode = 18446744073709551615

struct Node {
    kind   u8
    truth  bool
    number f64
    text   string
    key    string
    first  u64 // first child, or noNode
    next   u64 // next sibling, or noNode
    count  u64
}

struct Document {
    nodes collections.List[Node]
}

func (d *Document) Root() u64 {
    return 0
}

func (d *Document) Kind(at u64) u8 {
    return d.nodes.At(at).kind
}

func (d *Document) Number(at u64) f64 {
    return d.nodes.At(at).number
}

func (d *Document) Truth(at u64) bool {
    return d.nodes.At(at).truth
}

func (d *Document) Text(at u64) string {
    return d.nodes.At(at).text
}

func (d *Document) Len(at u64) u64 {
    return d.nodes.At(at).count
}

// The i-th element of an array, or the i-th member of an object.
func (d *Document) At(at u64, i u64) ?u64 {
    mut child := d.nodes.At(at).first
    mut seen u64 = 0
    for child != noNode {
        if seen == i {
            return child
        }
        seen += 1
        child = d.nodes.At(child).next
    }
    return nil
}

func (d *Document) KeyAt(at u64) string {
    return d.nodes.At(at).key
}

func (d *Document) Get(at u64, key string) ?u64 {
    mut child := d.nodes.At(at).first
    for child != noNode {
        if d.nodes.At(child).key == key {
            return child
        }
        child = d.nodes.At(child).next
    }
    return nil
}

// Convenience for the shape most callers want: a string member of an object.
func (d *Document) GetText(at u64, key string) string {
    if found := d.Get(at, key) {
        if d.nodes.At(found).kind == Str {
            return d.nodes.At(found).text
        }
    }
    return ""
}

func (d *Document) GetNumber(at u64, key string, fallback f64) f64 {
    if found := d.Get(at, key) {
        if d.nodes.At(found).kind == Number {
            return d.nodes.At(found).number
        }
    }
    return fallback
}

func (mut d *Document) Free() {
    d.nodes.Free()
}

struct parser {
    input []u8
    at    u64
    doc   Document
    a     mem.Allocator
}

func newNode(kind u8) Node {
    return Node{kind: kind, truth: false, number: 0.0, text: "", key: "",
                first: noNode, next: noNode, count: 0}
}

func (p *parser) done() bool {
    return p.at >= p.input.len
}

func (p *parser) peek() u8 {
    return p.input[p.at]
}

func (mut p *parser) spaces() {
    for !p.done() {
        c := p.peek()
        if c != 32 && c != 9 && c != 10 && c != 13 {
            return
        }
        p.at += 1
    }
}

func (mut p *parser) expect(c u8) !void {
    if p.done() || p.peek() != c {
        return error.BadJSON
    }
    p.at += 1
}

// Strings are returned as a window into the input when there is nothing to
// unescape, and copied into the allocator only when there is.
func (mut p *parser) text() !string {
    try p.expect(34)
    start := p.at
    mut escaped := false
    for !p.done() && p.peek() != 34 {
        if p.peek() == 92 {
            escaped = true
            p.at += 1
            if p.done() {
                return error.BadJSON
            }
        }
        p.at += 1
    }
    if p.done() {
        return error.BadJSON
    }
    raw := p.input[start..p.at]
    p.at += 1
    if !escaped {
        return string(raw)
    }

    mut out := try bytes.New(p.a, raw.len)
    mut i u64 = 0
    for i < raw.len {
        c := raw[i]
        if c != 92 {
            try out.WriteByte(c)
            i += 1
            continue
        }
        i += 1
        if i >= raw.len {
            return error.BadJSON
        }
        e := raw[i]
        i += 1
        if e == 110 {
            try out.WriteByte(10)
        } else if e == 116 {
            try out.WriteByte(9)
        } else if e == 114 {
            try out.WriteByte(13)
        } else if e == 98 {
            try out.WriteByte(8)
        } else if e == 102 {
            try out.WriteByte(12)
        } else if e == 117 {
            // \uXXXX, encoded as UTF-8. Surrogate pairs are left alone.
            if i + 4 > raw.len {
                return error.BadJSON
            }
            mut code u64 = 0
            mut k u64 = 0
            for k < 4 {
                code = code * 16 + u64(try hexDigit(raw[i + k]))
                k += 1
            }
            i += 4
            try writeRune(&out, code)
        } else {
            try out.WriteByte(e)
        }
    }
    return out.Str()
}

func hexDigit(c u8) !u8 {
    if c >= 48 && c <= 57 {
        return c - 48
    }
    if c >= 97 && c <= 102 {
        return c - 87
    }
    if c >= 65 && c <= 70 {
        return c - 55
    }
    return error.BadJSON
}

func writeRune(mut out *bytes.Buffer, code u64) !void {
    if code < 128 {
        try out.WriteByte(u8(code))
        return
    }
    if code < 2048 {
        try out.WriteByte(u8(192 + code / 64))
        try out.WriteByte(u8(128 + code % 64))
        return
    }
    try out.WriteByte(u8(224 + code / 4096))
    try out.WriteByte(u8(128 + (code / 64) % 64))
    try out.WriteByte(u8(128 + code % 64))
}

func (mut p *parser) number() !f64 {
    start := p.at
    if !p.done() && p.peek() == 45 {
        p.at += 1
    }
    for !p.done() {
        c := p.peek()
        digit := c >= 48 && c <= 57
        if !digit && c != 46 && c != 101 && c != 69 && c != 43 && c != 45 {
            break
        }
        p.at += 1
    }
    if p.at == start {
        return error.BadJSON
    }
    return try parseNumber(string(p.input[start..p.at]))
}

// Enough of the grammar for what a JSON document actually carries: an optional
// sign, digits, an optional fraction, an optional exponent.
func parseNumber(s string) !f64 {
    mut i u64 = 0
    mut sign f64 = 1.0
    if i < s.len && s[i] == 45 {
        sign = -1.0
        i += 1
    }
    mut whole f64 = 0.0
    mut digits u64 = 0
    for i < s.len && s[i] >= 48 && s[i] <= 57 {
        whole = whole * 10.0 + f64(s[i] - 48)
        i += 1
        digits += 1
    }
    if digits == 0 {
        return error.BadJSON
    }
    if i < s.len && s[i] == 46 {
        i += 1
        mut place f64 = 0.1
        for i < s.len && s[i] >= 48 && s[i] <= 57 {
            whole += f64(s[i] - 48) * place
            place *= 0.1
            i += 1
        }
    }
    if i < s.len && (s[i] == 101 || s[i] == 69) {
        i += 1
        mut negative := false
        if i < s.len && (s[i] == 43 || s[i] == 45) {
            negative = s[i] == 45
            i += 1
        }
        mut power u64 = 0
        for i < s.len && s[i] >= 48 && s[i] <= 57 {
            power = power * 10 + u64(s[i] - 48)
            i += 1
        }
        for k in 0..power {
            if negative {
                whole *= 0.1
            } else {
                whole *= 10.0
            }
        }
    }
    return sign * whole
}

func (mut p *parser) literal(word string, kind u8, truth bool) !u64 {
    if p.at + word.len > p.input.len {
        return error.BadJSON
    }
    if string(p.input[p.at..p.at+word.len]) != word {
        return error.BadJSON
    }
    p.at += word.len
    mut node := newNode(kind)
    node.truth = truth
    return try p.add(node)
}

func (mut p *parser) add(node Node) !u64 {
    at := p.doc.nodes.Len()
    try p.doc.nodes.Push(node)
    return at
}

func (mut p *parser) value() !u64 {
    p.spaces()
    if p.done() {
        return error.BadJSON
    }
    c := p.peek()

    if c == 123 {
        return try p.container(125, true)
    }
    if c == 91 {
        return try p.container(93, false)
    }
    if c == 34 {
        mut node := newNode(Str)
        node.text = try p.text()
        return try p.add(node)
    }
    if c == 116 {
        return try p.literal("true", Bool, true)
    }
    if c == 102 {
        return try p.literal("false", Bool, false)
    }
    if c == 110 {
        return try p.literal("null", Null, false)
    }
    mut node := newNode(Number)
    node.number = try p.number()
    return try p.add(node)
}

// Objects and arrays differ only in whether each element carries a key, so
// they share one loop.
func (mut p *parser) container(closing u8, keyed bool) !u64 {
    p.at += 1
    mut kind u8 = Array
    if keyed {
        kind = Object
    }
    at := try p.add(newNode(kind))
    mut last u64 = noNode
    mut count u64 = 0

    p.spaces()
    if !p.done() && p.peek() == closing {
        p.at += 1
        return at
    }

    for {
        p.spaces()
        mut key := ""
        if keyed {
            key = try p.text()
            p.spaces()
            try p.expect(58)
        }
        child := try p.value()
        p.doc.nodes.Set(child, withKey(p.doc.nodes.At(child), key))

        if last == noNode {
            p.doc.nodes.Set(at, withFirst(p.doc.nodes.At(at), child))
        } else {
            p.doc.nodes.Set(last, withNext(p.doc.nodes.At(last), child))
        }
        last = child
        count += 1

        p.spaces()
        if p.done() {
            return error.BadJSON
        }
        if p.peek() == 44 {
            p.at += 1
            continue
        }
        try p.expect(closing)
        break
    }

    p.doc.nodes.Set(at, withCount(p.doc.nodes.At(at), count))
    return at
}

func withKey(n Node, key string) Node {
    mut copy := n
    copy.key = key
    return copy
}

func withFirst(n Node, first u64) Node {
    mut copy := n
    copy.first = first
    return copy
}

func withNext(n Node, next u64) Node {
    mut copy := n
    copy.next = next
    return copy
}

func withCount(n Node, count u64) Node {
    mut copy := n
    copy.count = count
    return copy
}

func Parse(input []u8, mut a mem.Allocator) !Document {
    mut p := parser{input: input, at: 0,
                    doc: Document{nodes: try collections.NewList[Node](a, 16)},
                    a: a}
    root := try p.value()
    if root != 0 {
        return error.BadJSON
    }
    p.spaces()
    if !p.done() {
        return error.TrailingJSON
    }
    return p.doc
}

// --- writing ---------------------------------------------------------------

const MaxDepth = 32

// A streaming writer: it tracks whether the level it is in has had anything
// written yet, which is all it needs to put the commas in the right places.
struct Writer {
    out    *bytes.Buffer
    depth  u64
    filled [MaxDepth]bool
}

func NewWriter(mut out *bytes.Buffer) Writer {
    return Writer{out: out, depth: 0, filled: [MaxDepth]bool{}}
}

func (mut w *Writer) separate() !void {
    if w.filled[w.depth] {
        try w.out.WriteByte(44)
    }
    w.filled[w.depth] = true
}

func (mut w *Writer) open(c u8) !void {
    if w.depth + 1 >= MaxDepth {
        return error.TooDeep
    }
    try w.separate()
    try w.out.WriteByte(c)
    w.depth += 1
    w.filled[w.depth] = false
}

func (mut w *Writer) close(c u8) !void {
    if w.depth == 0 {
        return error.Unbalanced
    }
    w.depth -= 1
    try w.out.WriteByte(c)
}

func (mut w *Writer) BeginObject() !void {
    try w.open(123)
}

func (mut w *Writer) EndObject() !void {
    try w.close(125)
}

func (mut w *Writer) BeginArray() !void {
    try w.open(91)
}

func (mut w *Writer) EndArray() !void {
    try w.close(93)
}

// A key is written without a separator of its own: the value that follows
// belongs to the same comma.
func (mut w *Writer) Key(name string) !void {
    try w.separate()
    try fmt.Quote(w.out, name)
    try w.out.WriteByte(58)
    w.filled[w.depth] = false
}

func (mut w *Writer) Str(s string) !void {
    try w.separate()
    try fmt.Quote(w.out, s)
}

func (mut w *Writer) Int(v i64) !void {
    try w.separate()
    try fmt.I64(w.out, v)
}

func (mut w *Writer) Number(v f64, decimals u64) !void {
    try w.separate()
    try fmt.F64(w.out, v, decimals)
}

func (mut w *Writer) Bool(v bool) !void {
    try w.separate()
    try fmt.Bool(w.out, v)
}

func (mut w *Writer) Null() !void {
    try w.separate()
    try w.out.WriteString("null")
}
