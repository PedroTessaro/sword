package shapes

// Split across two files on purpose: within a package there are no headers
// and no declaration order, so this refers to Square before it is declared.
interface Shape {
    Area() i64
}

func TotalOf(a Shape, b Shape) i64 {
    return a.Area() + b.Area()
}

func UnitSquare() Square {
    return Square{side: 1}
}

// Exported so another package can name its members: `shapes.Corner.Round`.
enum Corner u8 {
    Sharp
    Round
    Bevelled
}
