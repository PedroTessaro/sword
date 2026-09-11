// expect: 42
// Constants fold at compile time, in any declaration order.

const BufferSize = MaxHeaders * 128
const MaxHeaders = 32
const Port = 8080
const Pi = 3.14159
const Greeting = "hi"
const Debug = false

func main() int {
    mut size u32 = BufferSize
    if size != 4096 || Port + 1 != 8081 {
        return 1
    }
    if Pi < 3.14 || Pi > 3.15 {
        return 2
    }
    if Debug || Greeting.len != 2 {
        return 3
    }
    return 42
}
