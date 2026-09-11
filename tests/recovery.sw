// expect-error: expected 'func', 'struct' or 'interface'
// A stray brace at file scope used to spin the parser forever.

}

func main() int {
    return 0
}
