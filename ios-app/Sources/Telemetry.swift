import AVFoundation
import Foundation
import UIKit

/// The phone's power and capture health, as one value: what rides the
/// STATE snapshot (docs/PROTOCOL.md §8, "Telemetry") so OBS can show an
/// operator that a camera is about to die, or is being throttled, before
/// the picture suffers.
///
/// Three separate facts, deliberately never merged:
///  - **Power**: battery percent, plug state, Low Power Mode.
///  - **`thermalState`**: the *whole device's* thermal condition
///    (`ProcessInfo.thermalState`).
///  - **`systemPressure`**: the *camera's* pressure
///    (`AVCaptureDevice.systemPressureState`) — the signal LensLink's
///    throttling already reacts to. It can be serious for a reason that
///    has nothing to do with heat (peak power), which is what `factors`
///    are for; calling every pressure event "thermal" would send an
///    operator to fan a phone that is actually low on battery.
///
/// Every field is optional and nil means *unknown* — never a made-up
/// nominal. The wire strings are the raw values below: stable English
/// tokens, never localized. Reporting only: nothing here changes what the
/// camera or encoder does.
struct StreamTelemetry: Equatable {
    enum BatteryState: String {
        case unplugged, charging, full, unknown

        init(_ state: UIDevice.BatteryState) {
            switch state {
            case .unplugged: self = .unplugged
            case .charging: self = .charging
            case .full: self = .full
            case .unknown: self = .unknown
            @unknown default: self = .unknown
            }
        }
    }

    enum ThermalState: String {
        case nominal, fair, serious, critical

        /// nil for a state this build predates: unknown beats a guess.
        init?(_ state: ProcessInfo.ThermalState) {
            switch state {
            case .nominal: self = .nominal
            case .fair: self = .fair
            case .serious: self = .serious
            case .critical: self = .critical
            @unknown default: return nil
            }
        }
    }

    enum SystemPressure: String {
        case nominal, fair, serious, critical, shutdown

        init?(_ level: AVCaptureDevice.SystemPressureState.Level) {
            switch level {
            case .nominal: self = .nominal
            case .fair: self = .fair
            case .serious: self = .serious
            case .critical: self = .critical
            case .shutdown: self = .shutdown
            default: return nil
            }
        }
    }

    /// Why the camera is pressured. Declared in the order they are sent,
    /// so an unchanged set always serializes identically.
    enum PressureFactor: String, CaseIterable {
        case systemTemperature
        case cameraTemperature
        case depthModuleTemperature
        case peakPower
        case batteryStress

        /// The `AVCaptureDevice.SystemPressureState.Factors` bit. Raw
        /// values rather than the named constants: `cameraTemperature`
        /// is iOS 17+ and `batteryStress` iOS 27+, and this app deploys
        /// to iOS 15 — a bit an older OS never sets simply never
        /// matches. (Values from AVCaptureSystemPressure.h.)
        fileprivate var bit: UInt {
            switch self {
            case .systemTemperature: return 1 << 0
            case .peakPower: return 1 << 1
            case .depthModuleTemperature: return 1 << 2
            case .cameraTemperature: return 1 << 3
            case .batteryStress: return 1 << 4
            }
        }

        /// The factors the platform reports, known ones only: a factor a
        /// future iOS adds is ignored here, as the plugin ignores names
        /// it doesn't know.
        static func factors(
            in factors: AVCaptureDevice.SystemPressureState.Factors
        ) -> [PressureFactor] {
            allCases.filter { factors.rawValue & $0.bit != 0 }
        }
    }

    /// 0–100, or nil when iOS won't say. Never -1, never clamped.
    var batteryPercent: Int?
    var batteryState: BatteryState?
    var lowPowerMode: Bool?
    var thermalState: ThermalState?
    var systemPressure: SystemPressure?
    var systemPressureFactors: [PressureFactor] = []

    /// The STATE keys for the known facts; unknown ones are left out
    /// (absent = unknown on the wire). `systemPressureFactors` is sent
    /// only when non-empty, to keep the snapshot small: an older plugin's
    /// STATE cache is finite (docs/PROTOCOL.md).
    var wireFields: [String: Any] {
        var fields: [String: Any] = [:]
        if let percent = batteryPercent, (0...100).contains(percent) {
            fields["batteryPercent"] = percent
        }
        if let batteryState { fields["batteryState"] = batteryState.rawValue }
        if let lowPowerMode { fields["lowPowerMode"] = lowPowerMode }
        if let thermalState { fields["thermalState"] = thermalState.rawValue }
        if let systemPressure {
            fields["systemPressure"] = systemPressure.rawValue
            if !systemPressureFactors.isEmpty {
                fields["systemPressureFactors"] =
                    systemPressureFactors.map { $0.rawValue }
            }
        }
        return fields
    }

    /// A short line for logs: `serious [cameraTemperature]`.
    static func describe(_ pressure: SystemPressure?,
                         _ factors: [PressureFactor]) -> String {
        guard let pressure else { return "unknown" }
        return factors.isEmpty
            ? pressure.rawValue
            : "\(pressure.rawValue) [\(factors.map { $0.rawValue }.joined(separator: ","))]"
    }
}
