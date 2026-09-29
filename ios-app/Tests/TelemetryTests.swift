// Platform-enum → wire-token mapping tests for Sources/Telemetry.swift.
// Run with ios-app/Tests/run-telemetry-tests.sh (needs Xcode and an iOS
// Simulator runtime). Opt-in like the plugin's unit tests, and independent
// of the machine's real battery: every platform value is constructed here.

import AVFoundation
import Foundation
import UIKit

var failures = 0
func check(_ cond: @autoclosure () -> Bool, _ what: String,
           line: Int = #line) {
    if !cond() {
        failures += 1
        FileHandle.standardError.write(
            Data("TelemetryTests.swift:\(line): FAILED: \(what)\n".utf8))
    }
}

typealias T = StreamTelemetry

// UIDevice.BatteryState → protocol battery state.
check(T.BatteryState(.unplugged) == .unplugged, "unplugged")
check(T.BatteryState(.charging) == .charging, "charging")
check(T.BatteryState(.full) == .full, "full")
check(T.BatteryState(.unknown) == .unknown, "unknown")
check(T.BatteryState(UIDevice.BatteryState(rawValue: 99)!) == .unknown,
      "a future battery state is unknown")

// ProcessInfo.ThermalState → protocol thermal string.
check(T.ThermalState(.nominal)?.rawValue == "nominal", "thermal nominal")
check(T.ThermalState(.fair)?.rawValue == "fair", "thermal fair")
check(T.ThermalState(.serious)?.rawValue == "serious", "thermal serious")
check(T.ThermalState(.critical)?.rawValue == "critical", "thermal critical")
check(T.ThermalState(ProcessInfo.ThermalState(rawValue: 99)!) == nil,
      "a future thermal state is unknown, not guessed")

// AVCaptureDevice pressure level → protocol string.
typealias Level = AVCaptureDevice.SystemPressureState.Level
check(T.SystemPressure(Level.nominal)?.rawValue == "nominal", "nominal")
check(T.SystemPressure(Level.fair)?.rawValue == "fair", "fair")
check(T.SystemPressure(Level.serious)?.rawValue == "serious", "serious")
check(T.SystemPressure(Level.critical)?.rawValue == "critical", "critical")
check(T.SystemPressure(Level.shutdown)?.rawValue == "shutdown", "shutdown")
check(T.SystemPressure(Level(rawValue: "meltdown")) == nil,
      "a future level is unknown")

// Pressure factors → protocol names, in their fixed order, ignoring bits
// this build doesn't know. The raw bits must also be the SDK's own.
typealias Factors = AVCaptureDevice.SystemPressureState.Factors
check(Factors.systemTemperature.rawValue == 1 << 0, "SDK systemTemperature")
check(Factors.peakPower.rawValue == 1 << 1, "SDK peakPower")
check(Factors.depthModuleTemperature.rawValue == 1 << 2, "SDK depth")
if #available(iOS 17.0, *) {
    check(Factors.cameraTemperature.rawValue == 1 << 3, "SDK cameraTemp")
}
if #available(iOS 27.0, *) {
    check(Factors.batteryStress.rawValue == 1 << 4, "SDK batteryStress")
}
func names(_ raw: UInt) -> [String] {
    T.PressureFactor.factors(in: Factors(rawValue: raw)).map { $0.rawValue }
}
check(names(0).isEmpty, "no factors")
check(names(1 << 3) == ["cameraTemperature"], "camera temperature")
check(names(1 << 1) == ["peakPower"], "peak power")
check(names(0b11111) == ["systemTemperature", "cameraTemperature",
                         "depthModuleTemperature", "peakPower",
                         "batteryStress"], "all known, in wire order")
check(names((1 << 9) | (1 << 1)) == ["peakPower"], "unknown bit ignored")
check(names(1 << 9).isEmpty, "only an unknown bit")

// Serialization: known facts only, honest about unknown.
var t = T()
check(t.wireFields.isEmpty, "an empty telemetry says nothing")

t = T(batteryPercent: 87, batteryState: .charging, lowPowerMode: false,
      thermalState: .nominal, systemPressure: .nominal)
let normal = t.wireFields
check(normal["batteryPercent"] as? Int == 87, "percent")
check(normal["batteryState"] as? String == "charging", "state")
check(normal["lowPowerMode"] as? Bool == false, "lpm false is sent")
check(normal["thermalState"] as? String == "nominal", "thermal")
check(normal["systemPressure"] as? String == "nominal", "pressure")
check(normal["systemPressureFactors"] == nil, "no empty factor array")
check(normal.count == 5, "exactly the five keys")

// Percent is 0...100 or absent — never -1, never clamped.
for bad in [-1, 101, 140, Int.max, Int.min] {
    t = T(batteryPercent: bad)
    check(t.wireFields["batteryPercent"] == nil, "\(bad) is not sent")
}
for good in [0, 1, 20, 21, 99, 100] {
    t = T(batteryPercent: good)
    check(t.wireFields["batteryPercent"] as? Int == good, "\(good) is sent")
}

// Low Power Mode is independent of the battery, and both truths are sent.
t = T(batteryPercent: 74, batteryState: .unplugged, lowPowerMode: true)
check(t.wireFields["lowPowerMode"] as? Bool == true, "lpm true")
t = T(batteryPercent: 5, batteryState: .unplugged)
check(t.wireFields["lowPowerMode"] == nil, "lpm is not inferred from 5%")

// Thermal state and system pressure are distinct keys.
t = T(thermalState: .serious)
check(t.wireFields["systemPressure"] == nil, "thermal does not imply pressure")
t = T(systemPressure: .serious, systemPressureFactors: [.peakPower])
check(t.wireFields["thermalState"] == nil, "pressure does not imply thermal")
check(t.wireFields["systemPressureFactors"] as? [String] == ["peakPower"],
      "factors ride with a level")
// Factors without a level we understood are dropped.
t = T(systemPressure: nil, systemPressureFactors: [.peakPower])
check(t.wireFields["systemPressureFactors"] == nil, "factors need a level")
t = T(systemPressure: .shutdown,
      systemPressureFactors: [.cameraTemperature, .systemTemperature])
check(t.wireFields["systemPressure"] as? String == "shutdown", "shutdown")

// The wire tokens are the protocol's, verbatim (docs/PROTOCOL.md §8).
check(T.BatteryState.unplugged.rawValue == "unplugged"
      && T.BatteryState.full.rawValue == "full"
      && T.BatteryState.unknown.rawValue == "unknown", "battery tokens")
check(T.describe(nil, []) == "unknown", "describe unknown")
check(T.describe(.serious, [.cameraTemperature, .peakPower])
      == "serious [cameraTemperature,peakPower]", "describe with factors")

// A real serialized snapshot is compact and re-parses.
t = T(batteryPercent: 100, batteryState: .unplugged, lowPowerMode: false,
      thermalState: .critical, systemPressure: .shutdown,
      systemPressureFactors: T.PressureFactor.allCases)
let data = try! JSONSerialization.data(withJSONObject: t.wireFields)
check(data.count < 260, "worst-case telemetry is \(data.count) bytes")
let back = try! JSONSerialization.jsonObject(with: data) as! [String: Any]
check(back["systemPressureFactors"] as? [String] ==
      T.PressureFactor.allCases.map { $0.rawValue }, "round trip")
print("worst-case telemetry fragment: \(data.count) bytes")

if failures == 0 {
    print("ok")
} else {
    print("\(failures) failure(s)")
    exit(1)
}
