// expect-error: no error named 'Tiemout' is declared
// The reason declarations exist. This used to compile, and the error it created
// was a different one from the error it was meant to be — so the handler that
// looked for `error.Timeout` never ran, and nothing said why.

error Timeout = "the peer did not answer in time"

func ask() !u64 {
    return error.Tiemout
}

func main() !int {
    n := ask() catch 0
    return int(n)
}
