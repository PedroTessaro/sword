// expect: 25
// One package, two files, imported from here by directory name.

import "shapes"

func main() int {
    mut a := shapes.NewSquare(3)
    mut b := shapes.NewSquare(4)
    mut unit := shapes.UnitSquare()
    return int(shapes.TotalOf(&a, &b)) - int(unit.Area()) + 1
}
