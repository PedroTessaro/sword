// expect-error: Tag does not satisfy Sized: no method 'Size'

interface Sized {
    Size() u64
}

struct Tag {
    id u64
}

func Measure[T: Sized](x *T) u64 {
    return x.Size()
}

func main() int {
    mut t := Tag{id: 9}
    return int(Measure(&t))
}
