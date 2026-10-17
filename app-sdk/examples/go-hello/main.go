// hello - K93OS app in Go (terminal/text backend; Go apps ship CGO-free so
// they run anywhere, which means they use the serial/VT console UI instead
// of the framebuffer toolkit).
package main

import "fmt"

func main() {
	fmt.Println("Hello from Go on K93OS-x86!")
	fmt.Println("This app is a plain GOOS=linux binary - sideload it with")
	fmt.Println("k93sdk bundle, then install the .ipa from Files or Safari.")
}
