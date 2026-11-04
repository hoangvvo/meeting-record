import Foundation

@main struct AudioProcessLifecycleTests {
    static func main() {
        let arguments = ["/bin/sleep", "30"].map { $0.withCString { strdup($0)! } }
        defer {
            for argument in arguments { free(argument) }
        }
        var argv: [UnsafeMutablePointer<CChar>?] = arguments + [nil]
        var environment: [UnsafeMutablePointer<CChar>?] = [nil]
        var child: pid_t = 0
        precondition(posix_spawn(&child, arguments[0], nil, nil, &argv, &environment) == 0)
        precondition(AudioProcessRegistry.isAlive(child))
        precondition(ProcessTree.identity(of: child) != nil)
        precondition(kill(child, SIGTERM) == 0)

        // Leave the child unreaped, as an embedding application's parent can do.
        // kill(pid, 0) still succeeds even though it can no longer produce audio.
        var exitInfo = siginfo_t()
        precondition(waitid(P_PID, id_t(child), &exitInfo, WEXITED | WNOWAIT) == 0)
        precondition(kill(child, 0) == 0)
        let zombieIsAlive = AudioProcessRegistry.isAlive(child)
        let zombieIdentity = ProcessTree.identity(of: child)
        var status: Int32 = 0
        precondition(waitpid(child, &status, 0) == child)
        precondition(!zombieIsAlive, "an exited, unreaped process must not keep capture alive")
        precondition(zombieIdentity == nil, "an exited process cannot remain a microphone target")
        precondition(!AudioProcessRegistry.isAlive(child))
        precondition(ProcessTree.identity(of: child) == nil)
        print("audio process lifecycle tests passed")
    }
}
