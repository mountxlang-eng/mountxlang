// Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
// web/test_wasm.js - Node.js test harness for MountX WebAssembly target

const fs = require('fs');
const path = require('path');

async function runTests() {
    console.log("=== [MountX WebAssembly Test Suite] ===");
    
    const wasmPath = path.join(__dirname, 'mountx.wasm');
    const jsPath = path.join(__dirname, 'mountx.js');
    
    if (!fs.existsSync(wasmPath) || !fs.existsSync(jsPath)) {
        console.error("Error: mountx.wasm or mountx.js not found in web/ directory.");
        process.exit(1);
    }
    
    console.log("Found WebAssembly artifact:", wasmPath, `(${fs.statSync(wasmPath).size} bytes)`);
    
    // Load Emscripten module
    const createMountX = require(jsPath);
    
    let capturedOutput = [];
    let capturedErrors = [];
    
    const Module = await createMountX({
        print: (text) => {
            capturedOutput.push(text);
            console.log("  [stdout]", text);
        },
        printErr: (text) => {
            capturedErrors.push(text);
            console.error("  [stderr]", text);
        }
    });
    
    console.log("WebAssembly module initialized successfully.");
    
    // Test 1: Basic printx and arithmetic
    console.log("\nTest 1: Basic printx and arithmetic...");
    capturedOutput = [];
    const test1_code = `mountx
let a = 20
let b = 22
printx("SUM: " + (a + b))
unmountx`;

    const res1 = Module.ccall('mountx_run', 'number', ['string'], [test1_code]);
    if (res1 !== 0 || !capturedOutput.some(l => l.includes("SUM: 42"))) {
        console.error("FAIL: Test 1 failed!", capturedOutput);
        process.exit(1);
    }
    console.log("PASS: Test 1 passed!");

    // Test 2: Function and recursion
    console.log("\nTest 2: Function and loops...");
    capturedOutput = [];
    const test2_code = `mountx
func factorial(n) {
    if (n <= 1) { return 1 }
    return n * factorial(n - 1)
}
printx("FACT: " + factorial(6))
unmountx`;

    const res2 = Module.ccall('mountx_run', 'number', ['string'], [test2_code]);
    if (res2 !== 0 || !capturedOutput.some(l => l.includes("FACT: 720"))) {
        console.error("FAIL: Test 2 failed!", capturedOutput);
        process.exit(1);
    }
    console.log("PASS: Test 2 passed!");

    // Test 3: OOP Class and inheritance
    console.log("\nTest 3: OOP Class & inheritance...");
    capturedOutput = [];
    const test3_code = `mountx
classx Shape {
    let name = ""
    func init(name) { this.name = name }
}
classx Square extends Shape {
    let side = 0
    func init(name, side) {
        super(name)
        this.side = side
    }
    func area() { return this.side * this.side }
}
let sq = newx Square("Box", 8)
printx("AREA: " + sq.area())
unmountx`;

    const res3 = Module.ccall('mountx_run', 'number', ['string'], [test3_code]);
    if (res3 !== 0 || !capturedOutput.some(l => l.includes("AREA: 64"))) {
        console.error("FAIL: Test 3 failed!", capturedOutput);
        process.exit(1);
    }
    console.log("PASS: Test 3 passed!");

    console.log("\n=============================================");
    console.log("ALL MOUNTX WEBASSEMBLY TESTS PASSED (100% OK)");
    console.log("=============================================");
    process.exit(0);
}

runTests().catch(err => {
    console.error("Fatal test error:", err);
    process.exit(1);
});
