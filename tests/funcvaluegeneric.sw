// expect-error: is generic, so there is no single function to point at
// A generic is a family of functions, and a value has to be one of them.

func identity[T](x T) T {
    return x
}

func main() int {
    f := identity
    return 0
}
