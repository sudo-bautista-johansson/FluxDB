package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/user/veldra-tools/tools/inspector"
)

func cmdServe(args []string) {
	serveCmd := flag.NewFlagSet("serve", flag.ExitOnError)
	portPtr := serveCmd.Int("port", 8080, "Port to listen on")

	if len(args) < 1 {
		fmt.Println("Usage: veldra serve [options] <database_file.vdb>")
		serveCmd.PrintDefaults()
		os.Exit(1)
	}

	serveCmd.Parse(args)
	files := serveCmd.Args()

	if len(files) < 1 {
		fmt.Println("Error: Missing database file argument")
		os.Exit(1)
	}

	dbPath := files[0]
	port := *portPtr

	fmt.Printf("Starting Veldra Inspector on http://localhost:%d for DB: %s\n", port, dbPath)
	
	// Start the actual HTTP server
	err := inspector.StartServer(port, dbPath)
	if err != nil {
		fmt.Printf("Server error: %v\n", err)
		os.Exit(1)
	}
}
