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

The module ships two applications:

- **`amd`** — blocks the session until a verdict is reached.
- **`amd_background`** — keeps analysing the call while the dialplan goes on
  (playing prompts, collecting digits, …) and reports the verdict
  asynchronously.

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

Events
------

When a detection decision is made, `mod_amd` fires a custom event (both
applications):

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
