import Foundation

struct ProcessTree {
    let parents: [pid_t: pid_t]
    let responsible: [pid_t: pid_t]

    init(parents: [pid_t: pid_t], responsible: [pid_t: pid_t] = [:]) {
        self.parents = parents
        self.responsible = responsible
    }

    // XPC services can have launchd as their parent. Resolve this optional
    // libSystem SPI at runtime, keeping normal ancestry usable if unavailable.
    private static let responsiblePID: (@convention(c) (pid_t) -> pid_t)? = {
        guard let handle = dlopen(nil, RTLD_LAZY) else { return nil }
        defer { dlclose(handle) }
        guard let symbol = dlsym(handle, "responsibility_get_pid_responsible_for_pid") else { return nil }
        return unsafeBitCast(symbol, to: (@convention(c) (pid_t) -> pid_t).self)
    }()

    static func identity(of pid: pid_t) -> UInt64? {
        var info = proc_bsdinfo()
        let size = Int32(MemoryLayout<proc_bsdinfo>.size)
        guard proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, size) == size else { return nil }
        return info.pbi_start_tvsec &* 1_000_000 &+ info.pbi_start_tvusec
    }

    static func read(ancestorsOf pids: [pid_t]) -> ProcessTree {
        var parents: [pid_t: pid_t] = [:]
        var responsible: [pid_t: pid_t] = [:]
        for pid in pids {
            if let owner = responsiblePID?(pid), owner > 1, owner != pid {
                responsible[pid] = owner
            }
            var current = pid
            while current > 0, parents[current] == nil {
                var info = proc_bsdinfo()
                let size = Int32(MemoryLayout<proc_bsdinfo>.size)
                guard proc_pidinfo(current, PROC_PIDTBSDINFO, 0, &info, size) == size else { break }
                let parent = pid_t(info.pbi_ppid)
                parents[current] = parent
                current = parent
            }
        }
        return ProcessTree(parents: parents, responsible: responsible)
    }

    func owner(of pid: pid_t, among candidates: Set<pid_t>) -> pid_t? {
        var current = pid
        var visited = Set<pid_t>()
        while current > 0, visited.insert(current).inserted {
            if candidates.contains(current) { return current }
            guard let parent = parents[current] else { break }
            current = parent
        }
        return nil
    }
}

struct ApplicationProcess {
    var pid: pid_t
    var bundleID: String
    var name: String
    var isRegular: Bool
}
