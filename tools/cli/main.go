package main

import (
	"fmt"
	"os"
)

func main() {
	fmt.Println("VeldraDB Toolkit v0.1")
	fmt.Println("---------------------")

	if len(os.Args) < 2 {
		printUsage()
		os.Exit(1)
	}

	command := os.Args[1]

	switch command {
	case "dump":
		cmdDump(os.Args[2:])
	case "query":
		cmdQuery(os.Args[2:])
	case "serve":
		cmdServe(os.Args[2:])
	case "import":
		cmdImport(os.Args[2:])
	default:
		fmt.Printf("Unknown command: %s\n", command)
		printUsage()
		os.Exit(1)
	}
}

func printUsage() {
	fmt.Println("Usage: veldra <command> [arguments]")
	fmt.Println("\nCommands:")
	fmt.Println("  dump    Dump contents of a Veldra database file")
	fmt.Println("  query   Execute a GQL query against a database")
	fmt.Println("  serve   Start the web-based Veldra Inspector UI")
	fmt.Println("  import  Import data from SQLite or JSON")
}

// Stub function for the query command
func cmdQuery(args []string) {
	fmt.Println("Running query command...")
	// Future: Use CGO to call the veldra_c_api from Go
}

// Stub function for the import command
func cmdImport(args []string) {
	fmt.Println("Running import command...")
}