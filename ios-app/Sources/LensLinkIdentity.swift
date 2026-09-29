import Foundation

/// The persistent identity of this LensLink installation: one random UUID,
/// generated once and then reused by the app and its screen-broadcast
/// extension alike. The OBS plugin binds a source to it, so the source
/// finds this phone again after a DHCP move, a rename, a Wi-Fi reconnect
/// or an OBS restart (docs/PROTOCOL.md, "Device identity").
///
/// It is deliberately *not* derived from anything about the hardware or
/// the network: no serial number, MAC or IP address, advertising id, or
/// `identifierForVendor`. It says "this installation", nothing more.
///
/// Storage: a small file in the App Group container both processes
/// share. It is created with an atomic create-if-absent, so when the app
/// and the extension start at the same moment exactly one UUID wins and
/// both read it. The file is excluded from backups — a backup restored
/// onto a second phone must not produce two phones with one identity —
/// so a restored or reinstalled app is a new installation that OBS
/// sources bind to afresh.
///
/// Without the App Group (an unsigned or re-signed build that dropped the
/// entitlement), the app keeps the same file in its own Application
/// Support directory, and the extension, which can't read that, reports
/// no identity rather than inventing a second one. When the group later
/// becomes available, the app carries that identity over.
enum LensLinkIdentity {
    /// Shared with the broadcast extension (ios-app/project.yml).
    static let appGroup = "group.com.exaltedpixels.LensLinkCamera"

    /// Canonical upper-case UUID string, or nil when this process has no
    /// usable storage for one (see above). Computed once per process.
    static let deviceID: String? = load()

    private static let fileName = "device-id"

    private static var isExtension: Bool {
        Bundle.main.bundlePath.hasSuffix(".appex")
    }

    private static func load() -> String? {
        let fm = FileManager.default
        let local = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)
            .first?.appendingPathComponent("LensLink", isDirectory: true)

        if let shared = fm.containerURL(forSecurityApplicationGroupIdentifier: appGroup) {
            let dir = shared.appendingPathComponent("LensLink", isDirectory: true)
            if let id = read(in: dir) {
                return id
            }
            // First run with the group: adopt an identity the app made
            // without it, so sources already bound to it keep working.
            let carried = isExtension ? nil : local.flatMap { read(in: $0) }
            return create(in: dir, id: carried ?? newID())
        }
        guard !isExtension, let local else { return nil }
        return read(in: local) ?? create(in: local, id: newID())
    }

    private static func newID() -> String {
        UUID().uuidString // upper case, 8-4-4-4-12
    }

    /// The stored id, if the file holds a valid one.
    private static func read(in dir: URL) -> String? {
        let url = dir.appendingPathComponent(fileName)
        guard let data = try? Data(contentsOf: url),
              let text = String(data: data, encoding: .utf8) else { return nil }
        return canonical(text.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// Stores `id` unless another process got there first; returns
    /// whichever id is on disk afterwards. The file is written whole to
    /// a temporary name and then hard-linked into place, which fails if
    /// the name exists — so a reader never sees a half-written file and
    /// two writers can't both win.
    private static func create(in dir: URL, id: String) -> String? {
        let fm = FileManager.default
        let url = dir.appendingPathComponent(fileName)
        let temp = dir.appendingPathComponent("\(fileName).\(UUID().uuidString).tmp")
        do {
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
            try Data(id.utf8).write(to: temp, options: .atomic)
            defer { try? fm.removeItem(at: temp) }
            do {
                try fm.linkItem(at: temp, to: url)
            } catch {
                guard fm.fileExists(atPath: url.path) else { throw error }
                if let winner = read(in: dir) {
                    return winner
                }
                // Present but unreadable or corrupt: it never was a
                // usable identity, so replace it.
                _ = try fm.replaceItemAt(url, withItemAt: temp)
            }
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            var excluded = url
            try? excluded.setResourceValues(values)
        } catch {
            print("LensLinkIdentity: could not store the device id: \(error)")
            return nil
        }
        return read(in: dir)
    }

    /// Accepts exactly a UUID and returns Foundation's canonical form.
    static func canonical(_ text: String) -> String? {
        guard text.count == 36, let uuid = UUID(uuidString: text) else { return nil }
        let id = uuid.uuidString
        guard id != "00000000-0000-0000-0000-000000000000" else { return nil }
        return id
    }
}
