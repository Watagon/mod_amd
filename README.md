mod\_amd — Asterisk AMD for FreeSWITCH
=======================================

`mod_amd` is a FreeSWITCH application module that implements Asterisk-style
answering machine detection (AMD) using voice activity detection (VAD). It
analyses the audio stream of an answered call in real time to determine whether
the remote party is a **human** or an **answering machine**.

This is particularly useful for outbound dialling scenarios such as predictive
diallers and automated call campaigns, where you want to route live humans to
agents and handle machine answers differently (e.g. leave a voicemail or hang
up).

The module ships two native applications plus a set of applications that are
interface compatible with the commercial `mod_com_amd`:

- **`amd`** — blocks the session until a verdict is reached.
- **`amd_background`** — keeps analysing the call while the dialplan goes on
  (playing prompts, collecting digits, …) and reports the verdict
  asynchronously.
- **`voice_start` / `voice_stop` / `waitforresult`** — the `mod_com_amd`
  compatible interface (see [mod_com_amd compatible interface](#mod_com_amd-compatible-interface)).
  Existing dialplans and scripts written for `mod_com_amd` work without the
  commercial licence.

How It Works
------------

Once the `amd` application is invoked on a channel, the module reads raw audio
frames and classifies each frame as either **voiced** or **silence** based on a
configurable energy threshold. It then tracks:

| Metric | Purpose |
|---|---|
| **Silence duration** | How long continuous silence has lasted |
| **Voice duration** | How long continuous speech has lasted |
| **Word count** | Number of distinct "words" (voice bursts separated by silence) |
| **Greeting phase** | Whether the caller has started speaking (transitioned out of initial silence) |

A decision is reached when one of the following conditions is met:

| Result | Cause | Condition |
|---|---|---|
| `MACHINE` | `INITIALSILENCE` | Silence lasted longer than `initial_silence` before any speech was detected |
| `MACHINE` | `LONGGREETING` | A single continuous utterance exceeded the `greeting` threshold |
| `MACHINE` | `MAXWORDLENGTH` | A single word exceeded `maximum_word_length` |
| `MACHINE` | `MAXWORDS` | The number of detected words reached `maximum_number_of_words` |
| `HUMAN` | `HUMAN` | After the greeting, silence lasted longer than `after_greeting_silence` (typical of a human saying "Hello?" and waiting) |
| `NOTSURE` | `TOOLONG` | The `total_analysis_time` expired without a definitive result |

Prerequisites
-------------

- **FreeSWITCH** installed with development headers
- **pkg-config** available and able to locate the FreeSWITCH `.pc` file
- **GCC** (or a compatible C compiler)

Building
--------

Point `pkg-config` at your FreeSWITCH installation and run `make`:

```bash
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig/
make
```

### Installing

Install the compiled module into the FreeSWITCH modules directory:

```bash
make install
```

You can override the destination with `DESTDIR`:

```bash
make install DESTDIR=/opt/freeswitch
```

After installing, load the module from the FreeSWITCH CLI:

```
freeswitch> load mod_amd
```

To load it automatically on startup, add the following to
`conf/autoload_configs/modules.conf.xml`:

```xml
<load module="mod_amd"/>
```

Configuration
-------------

Place the configuration file at **`conf/autoload_configs/amd.conf.xml`**.
Individual parameters may be omitted: the defaults below are applied for any
missing value, including when the file does not exist at all.

```xml
<configuration name="amd.conf" description="mod_amd Configuration">
  <settings>
    <param name="silence_threshold" value="256"/>
    <param name="maximum_word_length" value="5000"/>
    <param name="maximum_number_of_words" value="3"/>
    <param name="between_words_silence" value="50"/>
    <param name="min_word_length" value="100"/>
    <param name="total_analysis_time" value="5000"/>
    <param name="after_greeting_silence" value="800"/>
    <param name="greeting" value="1500"/>
    <param name="initial_silence" value="2500"/>
  </settings>
</configuration>
```

### Parameter Reference

| Parameter | Default | Description |
|---|---|---|
| `silence_threshold` | `256` | Energy threshold for classifying a frame as voiced vs. silent. Higher values require louder audio to be considered speech. |
| `initial_silence` | `2500` | Maximum silence (ms) allowed before any speech is detected. If exceeded, the result is `MACHINE` / `INITIALSILENCE`. |
| `greeting` | `1500` | Maximum duration (ms) of a single continuous utterance during the greeting phase. If exceeded, the result is `MACHINE` / `LONGGREETING`. |
| `after_greeting_silence` | `800` | Silence duration (ms) after the greeting that indicates a human. A human typically says "Hello?" and then waits. |
| `total_analysis_time` | `5000` | Total time (ms) allowed for analysis. If no decision is made within this window, the result is `NOTSURE` / `TOOLONG`. |
| `min_word_length` | `100` | Minimum duration (ms) of continuous voice to be considered a word. |
| `maximum_word_length` | `5000` | Maximum duration (ms) of a single word. If exceeded, the result is `MACHINE` / `MAXWORDLENGTH`. |
| `between_words_silence` | `50` | Silence duration (ms) between words that separates one word from the next. |
| `maximum_number_of_words` | `3` | Maximum number of words allowed. If exceeded, the result is `MACHINE` / `MAXWORDS`. |

All parameters are reloadable — you can update the configuration and reload
without restarting FreeSWITCH:

```
freeswitch> reloadxml
freeswitch> reload mod_amd
```

### `mod_com_amd` compatible parameters

`mod_com_amd` uses different names for the same knobs. `amd.conf` accepts both
families: a name only updates the interface it belongs to, so a native
`amd.conf` does not change the defaults of `voice_start` (and vice versa).
`silence_threshold` (aka `silent_threshold`) and `total_analysis_time` are
shared and update both.

The defaults of the compatible interface mirror the values shipped in
`mod_com_amd`'s `amd.conf.xml`, so `voice_start` behaves the same out of the
box:

| `mod_com_amd` name | Native name | Compat default | Notes |
|---|---|---|---|
| `silent_threshold` | `silence_threshold` | `256` | Shared |
| `silent_initial` | `initial_silence` | `4500` | Person / `silent-initial` |
| `silent_after_intro` | `after_greeting_silence` | `500` | Person / `silent-after-intro` |
| `noise_max_intro` | `greeting` | `1250` | Machine / `max-count` |
| `noise_min_length` | `min_word_length` | `120` | Minimum word length |
| `noise_inter_silence` | `between_words_silence` | `50` | Word boundary |
| `noise_max_count` | `maximum_number_of_words` | `3` | Machine / `max-count` |
| `total_analysis_time` | `total_analysis_time` | `5000` | Shared, milliseconds |
| – | `maximum_word_length` | `0` (disabled) | `mod_com_amd` has no such rule |
| `silent_max_session` | – | ignored | Accepted for compatibility |
| `ring_timeout` | – | ignored | Accepted for compatibility |
| `break_digit` | – | ignored | Accepted for compatibility |

`hangup_on_bug_fail` (default `true`), `event_fire` (default `false`) and
`debug` / `debug_loglevel` only exist in the compatible interface as well.
`hangup_on_bug_fail` hangs the call up when the media bug cannot be added, the
same way `mod_com_amd` does.

Applications
------------

### `amd` — blocking

```
amd
```

Reads the audio of the channel in the session thread and returns only when a
verdict (or `total_analysis_time`) is reached. Simple, but it holds the dialplan
for up to `total_analysis_time`.

### `amd_background` — non blocking

```
amd_background [start|stop]
```

`start` attaches a media bug to the channel and returns immediately: detection
runs in the media thread while the dialplan does something else. `stop` removes
the bug; a stopped detection has no verdict to report.

- Only one background detection runs per channel: a second `start` is refused
  and a `stop` without a running detection is a no-op.
- As soon as it has a verdict (or `total_analysis_time` expires) the detection
  publishes `amd_result` / `amd_cause` and the `CUSTOM amd::result` event, then
  removes its own media bug.
- The analysis runs on the **read leg**, i.e. it hears what the remote party
  sends. Audio that FreeSWITCH itself plays to the party (prompts, TTS) is on
  the write leg and is not analysed.

### `mod_com_amd` compatible interface

The commercial `mod_com_amd` module is not available on every installation
(it needs a licence). `mod_amd` implements its documented interface on top of
the same VAD, so a flow written for `mod_com_amd` keeps working:

| Application | Arguments | Description |
|---|---|---|
| `voice_start` | `[key=value,key=value,...]` | Starts one background detection per channel and returns immediately. Optional overrides of the settings, applied to this detection only. |
| `voice_stop` | – | Removes the media bug. A stopped detection has **no** verdict. |
| `waitforresult` | `[file]` | Answers the channel if needed and blocks until the detection has a verdict, playing `file` to pump the read leg. With no argument, or the literal word `silence`, it plays silence. |

```
<extension name="amd_compat">
  <condition field="destination_number" expression="^(\d+)$">
    <action application="answer"/>
    <action application="voice_start" data="silent_initial=6000,noise_max_count=4"/>
    <action application="waitforresult"/>
    <action application="log" data="INFO AMD: ${amd_status} (${amd_result})"/>
  </condition>
</extension>
```

- `voice_start` only attaches the media bug. The bug is driven by the session's
  read path, so the channel has to be busy: either run `waitforresult` (which
  plays silence) or keep the dialplan playing prompts / collecting input. An
  idle session that runs nothing does not read frames and the detection never
  progresses.
- One detection per channel: a second `voice_start` is refused with
  `AMD: Only 1 AMD per channel please!` and, with `hangup_on_bug_fail=true`
  (the default), the call is hung up — the same behaviour as `mod_com_amd`.
- Overrides are parsed before `voice_start` applies them, so they accept both
  name families, at most 20 pairs, case insensitive.
- `total_analysis_time` is the module timeout **and** the media bug's
  `stop_time`, so a verdict is published even if nothing pumps the read leg
  (the bug is pruned and publishes `too-long`). The verdict is timed in
  milliseconds by the media bug; `stop_time` is only a seconds resolution
  ceiling and is rounded up, so it never cuts a detection short. Keep
  `total_analysis_time` below the Event Socket command-response timeout (30 s
  by default) when `waitforresult` is driven from an event socket with the
  lock flag.
  - Unit note: `mod_com_amd` measures this budget in units of 1,000 bytes of
    decoded audio (about 62 ms at 8 kHz), so the packaged `5000` is roughly
    five minutes there, not five seconds. `mod_amd` uses **milliseconds** for
    both interfaces (and `0` means "no budget"), so a value tuned for
    `mod_com_amd`'s units has to be converted (e.g. `80` → `5000`).

#### Channel variables

`voice_start` clears the previous verdict; when the detection ends it sets:

| Variable | Values |
|---|---|
| `amd_status` | `person`, `machine`, `unsure`, `undefined` |
| `amd_result` | `silent-initial`, `silent-after-intro`, `max-count`, `too-long`, `media-bug-failed` |

`amd_status=undefined` / `amd_result=media-bug-failed` means the media bug
could not be added. If the channel hangs up before a verdict,
`waitforresult` sets `amd_wait_status` / `amd_wait_result` to `hangup`.

The mapping between the two interfaces:

| Native (`amd` / `amd_background`) | Compatible (`voice_start`) |
|---|---|
| `MACHINE` / `INITIALSILENCE` | `person` / `silent-initial` |
| `HUMAN` / `HUMAN` | `person` / `silent-after-intro` |
| `MACHINE` / `LONGGREETING` | `machine` / `max-count` |
| `MACHINE` / `MAXWORDLENGTH` | `machine` / `max-count` |
| `MACHINE` / `MAXWORDS` | `machine` / `max-count` |
| `NOTSURE` / `TOOLONG` | `unsure` / `too-long` |

Note that initial silence means a **person** in the compatible interface: a
human picks up and says nothing (or waits for the IVR), whereas an answering
machine starts its greeting. The native `amd` application keeps its original
Asterisk semantics where initial silence is a machine.

Setting `amd_debug` to a true value before `voice_start` turns on the debug
logging of that detection.

#### Actions on completion

`mod_com_amd` can run dialplan applications automatically when the verdict is
in. `mod_amd` honours the same channel variables: every variable whose name
begins with `amd_execute_on_<status>` is executed when the detection ends with
that status (`machine`, `person` or `unsure`). The value is an application and
an optional argument, and `${...}` variables in the argument are expanded when
the action runs (not when it is set).

```
<extension name="amd_execute_on">
  <condition field="destination_number" expression="^(\d+)$">
    <action application="set" data="amd_execute_on_machine=transfer machine_detected XML default"/>
    <action application="set" data="amd_execute_on_person=playback ${sound_prefix}/ivr/ivr-welcome_to_freeswitch.wav"/>
    <action application="voice_start"/>
    <action application="waitforresult"/>
    <action application="hangup"/>
  </condition>
</extension>
```

The actions are queued as session private commands with `event-lock`, so they
interrupt the application that is running when the verdict arrives (for
instance the `waitforresult` playback) the same way `mod_com_amd` does.

#### Events

When `event_fire=false` (the default) the verdict is queued to the session as a
`DETECTED_SPEECH` event, which an outbound Event Socket receives while
`divert_events` is on. The flag is per channel and stays set for the rest of
the call; an application that clears it (for example to avoid duplicate
delivery while it runs its own speech detection) has to set it again before
`voice_start`:

- **Event**: `DETECTED_SPEECH`
- **Headers**: `Speech-Type: detected-speech`, `amd_status`, `amd_result`
- **Body**: `amd_complete`

With `event_fire=true` the same event is fired globally with the channel
headers instead (so `myevents` filtering applies), and it carries
`Unique-ID` like any other channel event.

While the detection runs, `CUSTOM AMD::EVENT` is fired on every talk state
change with an `Action` header of `start-talking` or `stop-talking` (compatible
interface only).

#### APIs

| API | Output |
|---|---|
| `amd_available` | `true` (there is no licence server) |
| `amd_count` | `65535` |
| `amd_info` | `AMD license available => 65535`, `AMD license allocated => 0` |

These exist so monitoring / dialler code that probes `mod_com_amd` also works.

Channel Variables
-----------------

After a detection ends, two channel variables are set (by `amd`, and by
`amd_background` at the moment its detection finishes):

| Variable | Values | Description |
|---|---|---|
| `amd_result` | `HUMAN`, `MACHINE`, `NOTSURE` | The detection result |
| `amd_cause` | `HUMAN`, `INITIALSILENCE`, `LONGGREETING`, `MAXWORDLENGTH`, `MAXWORDS`, `TOOLONG` | The reason for the result |

These variables can be used in your dialplan or scripts to branch call logic
based on the detection outcome.

The compatible interface (`voice_start`) uses its own variables instead —
`amd_status` / `amd_result`, see
[mod_com_amd compatible interface](#mod_com_amd-compatible-interface).

Events
------

When a native detection decision is made, `mod_amd` fires a custom event (both
`amd` and `amd_background`):

- **Event**: `CUSTOM`
- **Subclass**: `amd::result`
- **Headers**:
  - `AMD-Result` — the detection result (`HUMAN`, `MACHINE`, or `NOTSURE`)
  - `AMD-Cause` — the cause of the result

You can subscribe to this event from ESL (Event Socket Library) or any
FreeSWITCH event consumer.

Usage Examples
--------------

### Dialplan — Basic AMD with Branching

Run AMD on an outbound call and branch based on the result:

```xml
<extension name="outbound_with_amd">
  <condition field="destination_number" expression="^(\d+)$">
    <!-- Bridge the call -->
    <action application="bridge" data="sofia/gateway/my_gateway/$1"/>

    <!-- Run AMD after the call is answered -->
    <action application="amd"/>

    <!-- Branch based on the result -->
    <action application="log" data="INFO AMD result: ${amd_result}, cause: ${amd_cause}"/>

    <action application="execute_extension" data="handle_${amd_result}"/>
  </condition>
</extension>

<extension name="handle_HUMAN">
  <condition field="destination_number" expression="^handle_HUMAN$">
    <action application="log" data="INFO Human detected — transferring to agent queue"/>
    <action application="transfer" data="agent_queue XML default"/>
  </condition>
</extension>

<extension name="handle_MACHINE">
  <condition field="destination_number" expression="^handle_MACHINE$">
    <action application="log" data="INFO Machine detected — playing voicemail message"/>
    <action application="playback" data="/usr/local/freeswitch/sounds/voicemail_message.wav"/>
    <action application="hangup"/>
  </condition>
</extension>

<extension name="handle_NOTSURE">
  <condition field="destination_number" expression="^handle_NOTSURE$">
    <action application="log" data="WARNING AMD could not determine — transferring to agent"/>
    <action application="transfer" data="agent_queue XML default"/>
  </condition>
</extension>
```

### Dialplan — Simple Inline Check

A simpler approach using inline conditions:

```xml
<extension name="amd_simple">
  <condition field="destination_number" expression="^(\d+)$">
    <action application="answer"/>
    <action application="amd"/>

    <!-- If machine, hang up -->
    <action application="log" data="INFO AMD: ${amd_result} (${amd_cause})"/>
  </condition>
</extension>

<extension name="amd_machine_hangup">
  <condition field="${amd_result}" expression="^MACHINE$">
    <action application="hangup" data="NORMAL_CLEARING"/>
  </condition>
</extension>
```

### Dialplan — Non Blocking Detection

Start the detection, keep running the IVR (playing prompts, collecting digits)
and check the result afterwards:

```xml
<extension name="amd_background">
  <condition field="destination_number" expression="^(\d+)$">
    <action application="answer"/>
    <action application="amd_background" data="start"/>

    <!-- Detection runs while these are executed -->
    <action application="playback" data="welcome.wav"/>
    <action application="sleep" data="1000"/>

    <action application="log" data="INFO AMD: ${amd_result} (${amd_cause})"/>
    <action application="amd_background" data="stop"/>
  </condition>
</extension>
```

The variables are only usable once the detection ends. To branch **while the
call goes on** subscribe to the `CUSTOM amd::result` event (see below) instead of
reading the variables, and jump to your machine/human handler from there.

### Lua Script

Use AMD from a Lua script:

```lua
-- Execute AMD (blocks until detection completes)
session:execute("amd")

-- Read the result
local result = session:getVariable("amd_result")
local cause  = session:getVariable("amd_cause")

freeswitch.consoleLog("INFO", "AMD result: " .. result .. ", cause: " .. cause .. "\n")

if result == "HUMAN" then
    session:execute("transfer", "agent_queue XML default")
elseif result == "MACHINE" then
    session:execute("playback", "/usr/local/freeswitch/sounds/voicemail_message.wav")
    session:hangup()
else
    -- NOTSURE — treat as human
    session:execute("transfer", "agent_queue XML default")
end
```

### ESL Event Subscription (Python)

Subscribe to AMD events using the FreeSWITCH Event Socket Library. This is also
how you get the result of an `amd_background` detection as soon as it is
available, without polling the channel variables:

```python
import ESL

con = ESL.ESLconnection("127.0.0.1", "8021", "ClueCon")

if con.connected():
    con.events("plain", "CUSTOM amd::result")

    while True:
        event = con.recvEvent()
        if event:
            uuid   = event.getHeader("Unique-ID")
            result = event.getHeader("AMD-Result")
            cause  = event.getHeader("AMD-Cause")
            print(f"Call {uuid}: {result} ({cause})")
```

License
-------

This project is open source. See the [LICENSE](LICENSE) file for details.
