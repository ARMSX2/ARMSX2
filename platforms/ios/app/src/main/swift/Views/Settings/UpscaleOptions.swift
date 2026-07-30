import Foundation

// One list for both the global Graphics screen and the per game Graphics tab.
// They used to keep separate copies and had already drifted: the per game one
// stopped at 4x while the global one went to 8x.
//
// The core has always stored this as a float, so the fractional steps below are
// a UI change only. They matter on a phone, where 2x often runs and 3x often
// does not, and the useful setting is somewhere in between.
enum UpscaleOptions {
    // Sizes are the 512x448 base times the multiplier. Every step lands on
    // whole pixels, which is why the list steps by a quarter and not a third.
    static let all: [(id: Float, title: String)] = [
        (0.25, "0.25x (Fastest)"),
        (0.5, "0.5x"),
        (0.75, "0.75x"),
        (1.0, "1x Native (512x448)"),
        (1.25, "1.25x (640x560)"),
        (1.5, "1.5x (768x672)"),
        (1.75, "1.75x (896x784)"),
        (2.0, "2x (1024x896)"),
        (2.25, "2.25x (1152x1008)"),
        (2.5, "2.5x (1280x1120)"),
        (2.75, "2.75x (1408x1232)"),
        (3.0, "3x (1536x1344)"),
        (3.5, "3.5x (1792x1568)"),
        (4.0, "4x (2048x1792)"),
        (5.0, "5x (2560x2240)"),
        (6.0, "6x (3072x2688)"),
        (8.0, "8x (4096x3584)")
    ]
}
