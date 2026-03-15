package inspector

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os/exec"
	"runtime"
	"time"
)

// StartServer initializes the HTTP router and starts listening securely on localhost
func StartServer(port int, dbPath string) error {
	
	// Serve static files from the UI directory 
	fs := http.FileServer(http.Dir("./tools/inspector/ui"))
	http.Handle("/", fs)

	// API Endpoint for Database Stats
	http.HandleFunc("/api/stats", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		
		stats := map[string]interface{}{
			"database": dbPath,
			"status": "online",
			"active_pages": 42,
			"entity_count": 10000,
			"archetypes": 3,
		}
		json.NewEncoder(w).Encode(stats)
	})

	// API Endpoint for running queries directly from the Web UI
	http.HandleFunc("/api/query", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
			return
		}

		var reqData map[string]string
		if err := json.NewDecoder(r.Body).Decode(&reqData); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		
		query := reqData["query"]

		var out []byte
		var err error
		
		// Run Native C++ Engine
		if runtime.GOOS == "windows" {
			cmd := exec.Command("cmd", "/c", "veldra_cli.exe", query)
			out, err = cmd.CombinedOutput()
		} else {
			cmd := exec.Command("./veldra_cli", query)
			out, err = cmd.CombinedOutput()
		}

		w.Header().Set("Content-Type", "application/json")
		response := map[string]interface{}{
			"success": err == nil,
			"message": string(out),
			"data": []interface{}{},
		}
		json.NewEncoder(w).Encode(response)
	})

	// Force connection strictly to 127.0.0.1 (Localhost, NO external networks)
	addr := fmt.Sprintf("127.0.0.1:%d", port)
	url := fmt.Sprintf("http://%s", addr)

	// MS Edge Native App Window Mode (Local Desktop App feeling)
	go func() {
		time.Sleep(500 * time.Millisecond) // Wait for server to bind
		var cmd *exec.Cmd
		if runtime.GOOS == "windows" {
			cmd = exec.Command("msedge", fmt.Sprintf("--app=%s", url))
		} else if runtime.GOOS == "darwin" {
			cmd = exec.Command("open", "-a", "Google Chrome", "--args", fmt.Sprintf("--app=%s", url))
		} else {
			cmd = exec.Command("google-chrome", fmt.Sprintf("--app=%s", url))
		}
		cmd.Start()
	}()

	fmt.Printf("Secure Local Engine running on %s\n", url)
	return http.ListenAndServe(addr, nil)
}
