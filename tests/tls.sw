// expect: 42
// expect-output: tls checked
// A handshake between two tasks in one program, then twenty at once, then the
// two answers that matter more than either: a certificate nobody signed is
// refused, and the same certificate is accepted once the client is told to trust
// it. A build without OpenSSL says so instead of pretending — hence the check at
// the top, which is also what a program shipping to an unknown machine wants.

import "std/fs"
import "std/io"
import "std/net"
import "std/tls"

const certPath = "/tmp/sword_tls_test_cert.pem"
const keyPath = "/tmp/sword_tls_test_key.pem"

// Echoes one message back and reports the protocol it ended up speaking.
func echo(l *net.Listener, ctx *tls.Context, mut served *atomic[u64],
          mut modern *atomic[u64]) !void {
    mut raw := l.Accept() catch return
    mut c := tls.Server(ctx, raw) catch return
    if c.Version() == "TLSv1.3" {
        modern.Add(1)
    }
    mut buf := [128]u8{}
    n := c.Read(buf[..]) catch 0
    c.Write(buf[0..n]) catch {}
    served.Add(1)
    c.Close()
}

func accepting(l *net.Listener, ctx *tls.Context, many u64,
               mut served *atomic[u64]) !void {
    scope {
        for i in 0..many {
            mut raw := l.Accept() catch break
            spawn handle(raw, ctx, served)
        }
    }
}

func handle(raw net.Conn, ctx *tls.Context, mut served *atomic[u64]) !void {
    mut c := tls.Server(ctx, raw) catch return
    mut buf := [128]u8{}
    n := c.Read(buf[..]) catch 0
    c.Write(buf[0..n]) catch {}
    if n > 0 {
        served.Add(1)
    }
    c.Close()
}

// Sends a line and checks it comes back. `ca` empty with verify on is the case
// that must fail: the server's certificate is signed by nobody.
func speak(port i32, verify bool, ca string, mut ok *atomic[u64]) !void {
    mut conf := tls.NewConfig()
    conf.Verify = verify
    conf.CAFile = ca
    mut ctx := tls.ClientContext(conf) catch return
    mut c := tls.Dial(&ctx, "127.0.0.1", port) catch {
        ctx.Free()
        return
    }
    c.WriteString("over tls") catch {
        c.Close()
        ctx.Free()
        return
    }
    mut back := [128]u8{}
    n := c.Read(back[..]) catch 0
    if string(back[0..n]) == "over tls" && c.Cipher().len > 0 {
        ok.Add(1)
    }
    c.Close()
    ctx.Free()
}

func main() !int {
    if !tls.Available() {
        // Nothing here can work, so what matters is that it fails rather than
        // half-works. Every entry point has to say the same thing.
        mut refused u64 = 0
        tls.SelfSigned("127.0.0.1", certPath, keyPath) catch {
            refused += 1
        }
        tls.ServerContext(certPath, keyPath) catch {
            refused += 1
        }
        tls.ClientContext(tls.NewConfig()) catch {
            refused += 1
        }
        if refused != 3 {
            return 1
        }
        try io.Print("tls checked: this build has no TLS\n")
        return 42
    }

    try tls.SelfSigned("127.0.0.1", certPath, keyPath)
    if !fs.Exists(certPath) || !fs.Exists(keyPath) {
        return 2
    }
    mut server := try tls.ServerContext(certPath, keyPath)

    // One connection, both ends in this program.
    mut served := atomic[u64](0)
    mut modern := atomic[u64](0)
    mut ok := atomic[u64](0)
    mut one := try net.Listen(0)
    scope {
        spawn echo(&one, &server, &served, &modern)
        spawn speak(one.Port(), false, "", &ok)
    }
    one.Close()
    if served.Load() != 1 || ok.Load() != 1 {
        return 3
    }
    // TLS 1.2 is the floor, so anything older is a misconfiguration; against
    // itself this should always land on 1.3.
    if modern.Load() != 1 {
        return 4
    }

    // Twenty at once. A handshake waits on the socket like any other read, so
    // twenty of them are twenty tasks and not twenty threads.
    mut crowd := try net.Listen(0)
    port := crowd.Port()
    mut many := atomic[u64](0)
    mut back := atomic[u64](0)
    scope {
        spawn accepting(&crowd, &server, 20, &many)
        for i in 0..20 {
            spawn speak(port, false, "", &back)
        }
    }
    crowd.Close()
    if many.Load() != 20 || back.Load() != 20 {
        return 5
    }

    // Verification on, nothing to verify against: refused.
    mut strict := try net.Listen(0)
    mut refused := atomic[u64](0)
    mut dropped := atomic[u64](0)
    scope {
        spawn echo(&strict, &server, &dropped, &modern)
        spawn speak(strict.Port(), true, "", &refused)
    }
    strict.Close()
    if refused.Load() != 0 {
        return 6
    }

    // The same certificate, now named as the one to trust: accepted.
    mut trusting := try net.Listen(0)
    mut trusted := atomic[u64](0)
    mut answered := atomic[u64](0)
    scope {
        spawn echo(&trusting, &server, &answered, &modern)
        spawn speak(trusting.Port(), true, certPath, &trusted)
    }
    trusting.Close()
    if trusted.Load() != 1 {
        return 7
    }

    server.Free()
    fs.Remove(certPath) catch {}
    fs.Remove(keyPath) catch {}

    try io.Print("tls checked: 22 handshakes, one refused on purpose\n")
    return 42
}
