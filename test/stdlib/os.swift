// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// REQUIRES: objc_interop

import os
import Foundation
import StdlibUnittest

defer { runAllTests() }

var osAPI = TestSuite("osAPI")

if #available(OSX 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *) {
  osAPI.test("log") {
    os_log("test: %d", 42)
    os_log("test2: %@", "test")
  }

  /* FIXME: currently disabled: rdar://problem/38354907
  osAPI.test("logData") {
    let data = "hello logging world".data(using: .utf8)!

    data.withUnsafeBytes { (bytes: UnsafePointer<CChar>) in
      os_log("%.3P", OpaquePointer(bytes))
      os_log("%.10P", OpaquePointer(bytes))
      os_log("%.*P", OpaquePointer(bytes))
    }
  }
  */
  osAPI.test("newLog") {
    let newLog = OSLog(subsystem: "com.apple.Swift", category: "Test")
    os_log("test", log: newLog)
  }
}

if #available(macOS 10.14, iOS 12.0, watchOS 5.0, tvOS 12.0, *) {
  osAPI.test("signpost") {
    let interestingLog = OSLog(subsystem: "com.apple.example.swift",
                                category: .pointsOfInterest)
    os_log(.info, log: interestingLog, "a=%d b=%d c=%d", 1, 2, 3)
    os_signpost(.event, log: interestingLog, name: "Basic Sans Message")
    os_signpost(.begin, log: interestingLog, name: "Basic Test", "%d", 42)
    os_signpost(.event, log: interestingLog, name: "Basic Test", "%d", 43)
    os_signpost(.end, log: interestingLog, name: "Basic Test", "%d", 44)
  }
}
