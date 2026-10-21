import Foundation

struct ProcessTree {
    let parents: [pid_t: pid_t]

    static func read(ancestorsOf pids: [pid_t]) -> ProcessTree {
        var parents: [pid_t: pid_t] = [:]
        for pid in pids {
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
        return ProcessTree(parents: parents)
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
