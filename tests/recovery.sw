// expect-error: expected 'func', 'struct', 'interface' or 'const'
// A stray brace at file scope used to spin the parser forever.

}

func main() int {
    return 0
}
