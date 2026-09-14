// expect: 42
// Fields are reordered for packing, so reading them back must go through the
// laid-out offsets, not the declared order.

struct Task {
    done bool
    id   u64
    prio u8
}

struct Pair {
    left  Task
    right Task
}

func total(p *Pair) u64 {
    return p.left.id + p.right.id
}

func main() int {
    mut pair := Pair{
        left:  Task{done: false, id: 20, prio: 1},
        right: Task{done: true, id: 15, prio: 2},
    }
    pair.left.id += 7
    return int(total(&pair))
}
