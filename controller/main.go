// amvdctl - ESL controller for mod_amvd (stdlib only).
//
// Listens for CUSTOM amvd::result / amvd::message_end and CHANNEL_HANGUP_COMPLETE,
// applies the per-call policy, accepts transcript verdicts from the voice agent
// (LiveKit AMD) and writes one JSON line per call for labelling, training and evaluation.
//
// Authority per class (avoids two detectors fighting):
//
//	fax, SIT, beep          -> mod_amvd (signal-level, final)
//	human vs machine        -> mod_amvd first pass
//	screener / IVR / a human that turns out to be voicemail -> transcript verdict from the agent
//	                           (may only move human->machine-ish, never machine->human after a drop)
//
// Policy per call comes from channel variable amvd_policy (set it in the originate string):
//
//	agent   human -> -human-cmd;  machine -> hang up
//	vmdrop  human -> -human-cmd;  machine -> wait for message_end -> -vm-cmd
//	detect  only log (dialplan or hooks act)
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/textproto"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

var (
	eslAddr   = flag.String("esl", "127.0.0.1:8021", "FreeSWITCH event socket address(es), comma-separated: one connection per node")
	eslPass   = flag.String("password", "ClueCon", "event socket password")
	logPath   = flag.String("log", "amvd_calls.jsonl", "per-call JSONL log (labelling / training input)")
	httpAddr  = flag.String("http", "127.0.0.1:8099", "listen address for transcript verdicts (empty = off)")
	humanCmd  = flag.String("human-cmd", "uuid_transfer {uuid} ai_agent XML default", "API command on human/unknown")
	vmCmd     = flag.String("vm-cmd", "uuid_transfer {uuid} amvd_vm_machine XML default", "API command at message end (vmdrop)")
	machCmd   = flag.String("machine-cmd", "uuid_kill {uuid} NORMAL_CLEARING", "API command on machine (agent policy)")
	faxCmd    = flag.String("fax-cmd", "uuid_kill {uuid} INCOMPATIBLE_DESTINATION", "API command on fax")
	sitCmd    = flag.String("sit-cmd", "uuid_kill {uuid} UNALLOCATED_NUMBER", "API command on SIT / unavailable")
	defPolicy = flag.String("policy", "agent", "default policy when amvd_policy is unset: agent|vmdrop|detect")
	verbose   = flag.Bool("v", false, "log every call (noisy at high volume)")
	callTTL   = flag.Duration("call-ttl", 15*time.Minute, "drop tracked calls whose hangup never arrived (ESL reconnects)")
)

// ---------------------------------------------------------------- ESL

type esl struct {
	conn net.Conn
	r    *textproto.Reader
	wmu  sync.Mutex
}

func dialESL(addr, pass string) (*esl, error) {
	c, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, err
	}
	e := &esl{conn: c, r: textproto.NewReader(bufio.NewReaderSize(c, 64*1024))}
	h, _, err := e.read()
	if err != nil || h.Get("Content-Type") != "auth/request" {
		c.Close()
		return nil, fmt.Errorf("unexpected greeting: %v %v", h, err)
	}
	e.send("auth " + pass)
	h, _, err = e.read()
	if err != nil || !strings.HasPrefix(h.Get("Reply-Text"), "+OK") {
		c.Close()
		return nil, fmt.Errorf("auth failed: %s %v", h.Get("Reply-Text"), err)
	}
	// CUSTOM must come last: FreeSWITCH reads every word after it as a subclass, so a
	// CHANNEL_HANGUP_COMPLETE placed after it is never subscribed.
	e.send("event plain CHANNEL_HANGUP_COMPLETE CUSTOM amvd::result amvd::message_end")
	// Filters are OR-ed: amvd events, plus hangups of channels mod_amvd marked. Without this
	// every hangup on a 5000-CC node would cross the socket.
	e.send("filter Event-Subclass amvd::result")
	e.send("filter Event-Subclass amvd::message_end")
	e.send("filter variable_amvd_active true")
	return e, nil
}

func (e *esl) send(cmd string) {
	e.wmu.Lock()
	defer e.wmu.Unlock()
	fmt.Fprintf(e.conn, "%s\n\n", cmd)
}

func (e *esl) api(cmd string) { e.send("bgapi " + cmd) }

func (e *esl) read() (textproto.MIMEHeader, []byte, error) {
	h, err := e.r.ReadMIMEHeader()
	if err != nil {
		return nil, nil, err
	}
	var body []byte
	if cl := h.Get("Content-Length"); cl != "" {
		n, _ := strconv.Atoi(cl)
		body = make([]byte, n)
		if _, err := io.ReadFull(e.r.R, body); err != nil {
			return nil, nil, err
		}
	}
	return h, body, nil
}

// event-plain bodies are "Key: url-encoded value" lines
func parseEvent(body []byte) map[string]string {
	ev := map[string]string{}
	for _, line := range strings.Split(string(body), "\n") {
		k, v, ok := strings.Cut(line, ": ")
		if !ok {
			continue
		}
		if dv, err := url.QueryUnescape(v); err == nil {
			v = dv
		}
		ev[k] = v
	}
	return ev
}

// ---------------------------------------------------------------- calls

type callRec struct {
	UUID          string            `json:"uuid"`
	Campaign      string            `json:"campaign,omitempty"`
	Destination   string            `json:"destination,omitempty"`
	Policy        string            `json:"policy"`
	Profile       string            `json:"profile"`
	Model         string            `json:"model"`
	Result        string            `json:"result"`
	Reason        string            `json:"reason"`
	DecisionMS    int               `json:"decision_ms"`
	PMachine      float64           `json:"p_machine"`
	FirstSpeechMS int               `json:"first_speech_ms"`
	Features      string            `json:"features"`
	EndType       string            `json:"end_type,omitempty"`
	EndMS         int               `json:"end_ms,omitempty"`
	BeepHz        string            `json:"beep_hz,omitempty"`
	Gateway       string            `json:"gateway,omitempty"`
	RingMS        int               `json:"ring_ms"`                  // channel created -> answered; very short = suspect FAS
	EarlyMedia    bool              `json:"early_media"`              // 183/early media before answer
	NoAudioMS     int               `json:"no_audio_ms"`              // audio that was missing RTP
	AgentDelayMS  *int              `json:"agent_delay_ms,omitempty"` // result -> agent's first audio (POST /v1/agent-audio)
	Transcript    *transcriptVerdct `json:"transcript,omitempty"`
	Final         string            `json:"final"` // what we acted on
	Actions       []string          `json:"actions"`
	HangupCause   string            `json:"hangup_cause,omitempty"`
	BillSec       int               `json:"billsec,omitempty"`
	AnsweredAt    string            `json:"answered_at,omitempty"`
	Truth         string            `json:"truth"` // left empty: fill by QA / disposition for training
	Node          string            `json:"node"`

	node     *esl
	seen     time.Time
	resultAt time.Time
}

type transcriptVerdct struct {
	Category   string `json:"category"` // human|machine-vm|machine-ivr|machine-unavailable|screener|uncertain
	Transcript string `json:"transcript,omitempty"`
	AtMS       int    `json:"at_ms,omitempty"`
}

type controller struct {
	mu    sync.Mutex
	calls map[string]*callRec
	out   *json.Encoder
	outW  *bufio.Writer
	outMu sync.Mutex
}

func atoi(s string) int { n, _ := strconv.Atoi(s); return n }
func atof(s string) float64 {
	f, _ := strconv.ParseFloat(s, 64)
	return f
}

func fill(tpl string, c *callRec) string {
	r := strings.NewReplacer("{uuid}", c.UUID, "{result}", c.Result, "{reason}", c.Reason, "{campaign}", c.Campaign)
	return r.Replace(tpl)
}

func (k *controller) act(c *callRec, tpl, why string) {
	if tpl == "" {
		return
	}
	cmd := fill(tpl, c)
	c.Actions = append(c.Actions, why+": "+cmd)
	if c.node != nil {
		c.node.api(cmd) // commands go back to the node that owns the channel
	}
}

func (k *controller) onResult(node *esl, addr string, ev map[string]string) {
	uuid := ev["Unique-ID"]
	k.mu.Lock()
	defer k.mu.Unlock()
	c := k.calls[uuid]
	if c == nil {
		c = &callRec{UUID: uuid}
		k.calls[uuid] = c
	}
	c.node, c.Node, c.seen = node, addr, time.Now()
	c.Policy = ev["variable_amvd_policy"]
	if c.Policy == "" {
		c.Policy = *defPolicy
	}
	c.Campaign = ev["variable_amvd_campaign"]
	c.Destination = ev["Caller-Destination-Number"]
	c.Profile, c.Model = ev["AMVD-Profile"], ev["AMVD-Model"]
	c.Result, c.Reason = ev["AMVD-Result"], ev["AMVD-Reason"]
	c.DecisionMS, c.PMachine = atoi(ev["AMVD-Decision-MS"]), atof(ev["AMVD-P-Machine"])
	c.FirstSpeechMS, c.Features = atoi(ev["AMVD-First-Speech-MS"]), ev["AMVD-Features"]
	c.Gateway, c.RingMS = ev["AMVD-Gateway"], atoi(ev["AMVD-Ring-MS"])
	c.EarlyMedia, c.NoAudioMS = ev["AMVD-Early-Media"] == "true", atoi(ev["AMVD-No-Audio-MS"])
	c.resultAt = time.Now()
	c.Final = c.Result
	if *verbose {
		log.Printf("%s result=%s reason=%s %dms policy=%s gw=%s ring=%dms", uuid, c.Result, c.Reason, c.DecisionMS, c.Policy, c.Gateway, c.RingMS)
	}

	if c.Policy == "detect" {
		return
	}
	switch c.Result {
	case "human", "unknown":
		k.act(c, *humanCmd, "human")
	case "machine":
		if c.Policy == "agent" {
			k.act(c, *machCmd, "machine")
		} // vmdrop: wait for message_end
	case "fax":
		k.act(c, *faxCmd, "fax")
	case "unavailable":
		k.act(c, *sitCmd, "sit")
	}
}

func (k *controller) onMessageEnd(ev map[string]string) {
	uuid := ev["Unique-ID"]
	k.mu.Lock()
	defer k.mu.Unlock()
	c := k.calls[uuid]
	if c == nil {
		return
	}
	c.EndType, c.EndMS, c.BeepHz = ev["AMVD-End-Type"], atoi(ev["AMVD-End-MS"]), ev["AMVD-Beep-Hz"]
	if *verbose {
		log.Printf("%s message_end=%s at %dms", uuid, c.EndType, c.EndMS)
	}
	if c.Policy == "vmdrop" && c.Final == "machine" {
		k.act(c, *vmCmd, "vm_drop_"+c.EndType)
	}
}

// Transcript verdict from the voice agent (e.g. LiveKit AMD result forwarded by the agent worker).
func (k *controller) onTranscript(uuid string, v transcriptVerdct) error {
	k.mu.Lock()
	defer k.mu.Unlock()
	c := k.calls[uuid]
	if c == nil {
		return fmt.Errorf("unknown call")
	}
	c.Transcript = &v
	// Only human -> machine corrections; the agent then leaves the message itself
	// (it already owns the media), so we record rather than transfer.
	if c.Final == "human" || c.Final == "unknown" {
		switch v.Category {
		case "machine-vm", "machine-unavailable":
			c.Final = v.Category
			c.Actions = append(c.Actions, "transcript override: "+v.Category)
		case "screener", "machine-ivr":
			c.Final = v.Category // agent should answer the screener / navigate the IVR
		}
	}
	return nil
}

// The voice agent reports when it starts speaking; the gap since the verdict is dead air the
// callee hears (long gaps are what make people repeat "hello? hello?"). First report wins.
func (k *controller) onAgentAudio(uuid string) error {
	k.mu.Lock()
	defer k.mu.Unlock()
	c := k.calls[uuid]
	if c == nil {
		return fmt.Errorf("unknown call")
	}
	if c.AgentDelayMS == nil && !c.resultAt.IsZero() {
		ms := int(time.Since(c.resultAt).Milliseconds())
		c.AgentDelayMS = &ms
	}
	return nil
}

func (k *controller) onHangup(ev map[string]string) {
	uuid := ev["Unique-ID"]
	k.mu.Lock()
	c := k.calls[uuid]
	delete(k.calls, uuid)
	k.mu.Unlock()
	if c == nil {
		return
	}
	c.HangupCause = ev["Hangup-Cause"]
	c.BillSec = atoi(ev["variable_billsec"])
	if us := atoi(ev["Caller-Channel-Answered-Time"]); us > 0 {
		c.AnsweredAt = time.UnixMicro(int64(us)).UTC().Format(time.RFC3339Nano)
	}
	k.write(c)
}

func (k *controller) write(c *callRec) {
	if c.Actions == nil {
		c.Actions = []string{}
	}
	k.outMu.Lock()
	k.out.Encode(c)
	k.outMu.Unlock()
}

// buffered log: one syscall per second instead of one per call
func (k *controller) flusher() {
	for range time.Tick(time.Second) {
		k.outMu.Lock()
		k.outW.Flush()
		k.outMu.Unlock()
	}
}

// calls whose hangup never arrived (node restart, socket drop) must not grow the map forever
func (k *controller) sweeper() {
	for range time.Tick(time.Minute) {
		cut := time.Now().Add(-*callTTL)
		var stale []*callRec
		k.mu.Lock()
		for id, c := range k.calls {
			if c.seen.Before(cut) {
				stale = append(stale, c)
				delete(k.calls, id)
			}
		}
		n := len(k.calls)
		k.mu.Unlock()
		for _, c := range stale {
			c.HangupCause = "UNKNOWN_EXPIRED"
			k.write(c)
		}
		log.Printf("tracking %d calls, expired %d", n, len(stale))
	}
}

func (k *controller) serveHTTP() {
	mux := http.NewServeMux()
	// POST /v1/transcript-verdict {"uuid":"...","category":"machine-vm","transcript":"...","at_ms":1800}
	mux.HandleFunc("/v1/transcript-verdict", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		var body struct {
			UUID string `json:"uuid"`
			transcriptVerdct
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 1<<16)).Decode(&body); err != nil || body.UUID == "" {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		if err := k.onTranscript(body.UUID, body.transcriptVerdct); err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
	// POST /v1/agent-audio {"uuid":"..."} when the agent plays its first audio on the call
	mux.HandleFunc("/v1/agent-audio", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		var body struct {
			UUID string `json:"uuid"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 1<<16)).Decode(&body); err != nil || body.UUID == "" {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		if err := k.onAgentAudio(body.UUID); err != nil {
			http.Error(w, err.Error(), http.StatusNotFound)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) { io.WriteString(w, "ok\n") })
	log.Printf("transcript verdicts on http://%s/v1/transcript-verdict", *httpAddr)
	log.Fatal(http.ListenAndServe(*httpAddr, mux))
}

func main() {
	flag.Parse()
	f, err := os.OpenFile(*logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o640)
	if err != nil {
		log.Fatal(err)
	}
	w := bufio.NewWriterSize(f, 256*1024)
	k := &controller{calls: map[string]*callRec{}, out: json.NewEncoder(w), outW: w}
	go k.flusher()
	go k.sweeper()
	if *httpAddr != "" {
		go k.serveHTTP()
	}
	var wg sync.WaitGroup
	for _, addr := range strings.Split(*eslAddr, ",") {
		addr = strings.TrimSpace(addr)
		if addr == "" {
			continue
		}
		wg.Add(1)
		go func(addr string) { defer wg.Done(); k.runNode(addr) }(addr)
	}
	wg.Wait()
}

// one goroutine per FreeSWITCH node; reconnects forever
func (k *controller) runNode(addr string) {
	for {
		e, err := dialESL(addr, *eslPass)
		if err != nil {
			log.Printf("[%s] esl: %v (retry in 2s)", addr, err)
			time.Sleep(2 * time.Second)
			continue
		}
		log.Printf("[%s] connected", addr)
		for {
			h, body, err := e.read()
			if err != nil {
				log.Printf("[%s] esl read: %v", addr, err)
				break
			}
			if h.Get("Content-Type") != "text/event-plain" {
				continue // command/reply, api/response, disconnect notices
			}
			ev := parseEvent(body)
			switch ev["Event-Name"] {
			case "CUSTOM":
				switch ev["Event-Subclass"] {
				case "amvd::result":
					k.onResult(e, addr, ev)
				case "amvd::message_end":
					k.onMessageEnd(ev)
				}
			case "CHANNEL_HANGUP_COMPLETE":
				k.onHangup(ev)
			}
		}
		e.conn.Close()
		time.Sleep(time.Second)
	}
}
