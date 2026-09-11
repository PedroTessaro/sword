// expect-error: is defined in terms of itself

const A = B + 1
const B = A + 1

func main() int {
    return A
}
