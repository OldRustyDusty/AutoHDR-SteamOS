// SPDX-License-Identifier: MIT
//
// HDR Ket quick access panel.
//
// The panel works on one game at a time: the game that is running, or else
// the game whose library page is open. It turns HDR Ket on for that game
// through its launch options, and edits the game's profile: live through the
// running layer, or in the profile file when the game isn't running. Every
// change is saved automatically.

import {
  ButtonItem,
  ConfirmModal,
  DialogButton,
  DropdownItem,
  Field,
  Focusable,
  ModalRoot,
  PanelSection,
  PanelSectionRow,
  Router,
  SliderField,
  ToggleField,
  showModal,
  staticClasses,
} from "@decky/ui";
import { callable, definePlugin } from "@decky/api";
import { useEffect, useRef, useState } from "react";
import { MdHdrOn } from "react-icons/md";

import { addHdrKet, hasHdrKet, removeHdrKet } from "./launchOptions";

// ---------------------------------------------------------------------------
// Backend (main.py)

interface Settings {
  peak_nits: number;
  peak_override: boolean;
  middle_grey_nits: number;
  paper_white_nits: number;
  expansion: number;
  saturation: number;
  black_floor: number;
  eotf: string;
  tonemap: string;
  gamut_expansion: number;
  adapt_strength: number;
  adapt_release: number;
  shoulder_start: number;
  shoulder_pow: number;
  split?: number;
  pattern?: boolean;
  bypass?: boolean;
}

interface Stats {
  frames: number;
  peak: number;
  avg: number;
  scene_white: number;
  shadow_lift: number;
  tonemap_source: number;
  wide_gamut: number;
}

interface Display {
  name: string;
  key: string;
  edid_peak: number;
  default_peak: number;
  configured: boolean;
  count: number;
}

interface Game {
  appid: number;
  live: boolean;
  port?: number;
  url?: string;
  has_profile?: boolean;
  settings: Settings;
  stats?: Stats;
  output?: { format: string; colorspace: string };
}

interface Overview {
  layer: { state: "installed" | "developer" | "missing"; path: string };
  display: Display;
  game: Game | null;
}

type Value = number | string | boolean;

const getOverview = callable<[appid: number], Overview>("get_overview");
const setSettings = callable<[appid: number, changes: Record<string, Value>], Overview>("set_settings");
const resetGame = callable<[appid: number], Overview>("reset_game");
const setDefaultPeak = callable<[nits: number, appid: number], Overview>("set_default_peak");

// ---------------------------------------------------------------------------
// Steam client

interface Target {
  appid: number;
  name: string;
  running: boolean;
}

function currentTarget(): Target | null {
  const running = Router.MainRunningApp;
  if (running?.appid) {
    return { appid: Number(running.appid), name: running.display_name, running: true };
  }
  try {
    const path: string | undefined = (window as any).SteamUIStore?.WindowStore?.GamepadUIMainWindowInstance
      ?.LocationPathName;
    const match = path?.match(/\/library\/app\/(\d+)/);
    if (match) {
      const appid = Number(match[1]);
      const name = (window as any).appStore?.GetAppOverviewByAppID?.(appid)?.display_name ?? `App ${appid}`;
      return { appid, name, running: false };
    }
  } catch {
    // Steam internals moved; the running game still works.
  }
  return null;
}

function getLaunchOptions(appid: number): Promise<string> {
  return new Promise((resolve) => {
    const apps = (window as any).SteamClient?.Apps;
    if (!apps?.RegisterForAppDetails) return resolve("");
    let done = false;
    let unregister = () => {};
    const finish = (value: string) => {
      if (done) return;
      done = true;
      unregister();
      resolve(value);
    };
    const timer = setTimeout(() => finish(""), 1500);
    const registration = apps.RegisterForAppDetails(appid, (details: any) => {
      clearTimeout(timer);
      finish(typeof details?.strLaunchOptions === "string" ? details.strLaunchOptions : "");
    });
    unregister = () => registration?.unregister?.();
  });
}

function setLaunchOptions(appid: number, options: string) {
  (window as any).SteamClient?.Apps?.SetAppLaunchOptions?.(appid, options);
}

// ---------------------------------------------------------------------------
// Display peak dialog: first run, and "Change default peak" later.

function PeakModal(props: { display: Display; firstRun: boolean; closeModal?: () => void; onSave: (nits: number) => void }) {
  const { display, firstRun } = props;
  const suggested = display.configured ? display.default_peak : display.edid_peak || 1000;
  const [nits, setNits] = useState(suggested);
  const save = (value: number) => {
    props.onSave(value);
    props.closeModal?.();
  };
  return (
    <ModalRoot onCancel={props.closeModal} closeModal={props.closeModal}>
      <div className={staticClasses.Title} style={{ marginBottom: 8 }}>
        {firstRun ? "Set up HDR Ket" : "Default peak brightness"}
      </div>
      <p>
        {display.name}
        {display.edid_peak > 0
          ? ` reports a peak brightness of ${display.edid_peak} nits.`
          : " doesn't report its peak brightness. Most HDR TVs reach 600 to 1500 nits; 1000 is a safe start."}
      </p>
      <SliderField
        label="Peak brightness"
        value={nits}
        min={200}
        max={4000}
        step={10}
        showValue
        editableValue
        valueSuffix=" nits"
        onChange={setNits}
      />
      <p style={{ fontSize: "0.85em", opacity: 0.8 }}>
        Used by every game on this display unless a game sets its own. To measure it, turn on the test pattern while
        a game runs with HDR Ket: your peak is the last bar that still looks brighter than the one before it.
      </p>
      <Focusable style={{ display: "flex", gap: 8, marginTop: 12 }} flow-children="horizontal">
        {display.edid_peak > 0 && nits !== display.edid_peak && (
          <DialogButton onClick={() => save(display.edid_peak)}>Use {display.edid_peak} nits</DialogButton>
        )}
        <DialogButton onClick={() => save(nits)}>
          {firstRun ? "Confirm" : "Save"} {nits} nits
        </DialogButton>
      </Focusable>
    </ModalRoot>
  );
}

// ---------------------------------------------------------------------------
// Panel

const pct = (v: number) => Math.round(v * 100);

const TONEMAP_OPTIONS = [
  { data: "off", label: "Off" },
  { data: "clip", label: "Clip" },
  { data: "map", label: "Map" },
  { data: "adaptive", label: "Adaptive" },
];
const EOTF_OPTIONS = [
  { data: "linear", label: "Linear" },
  { data: "srgb", label: "sRGB" },
  { data: "gamma22", label: "Gamma 2.2" },
  { data: "gamma24", label: "Gamma 2.4" },
];
const COMPARE_OPTIONS = [
  { data: "hdr", label: "HDR" },
  { data: "split", label: "Split (SDR on the left)" },
  { data: "sdr", label: "SDR" },
];

let firstRunShown = false; // once per Steam session; it returns until confirmed

function Content() {
  const [target, setTarget] = useState<Target | null>(currentTarget());
  const [overview, setOverview] = useState<Overview | null>(null);
  const [enabled, setEnabled] = useState<boolean | null>(null);
  const [advanced, setAdvanced] = useState(false);
  // Values being edited win over what the backend last reported, so a slider
  // doesn't jump back while it is being dragged.
  const pending = useRef<Record<string, { value: Value; at: number }>>({});
  const queued = useRef<Record<string, Value>>({});
  const sendTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const appid = target?.appid ?? 0;

  useEffect(() => {
    let alive = true;
    const tick = async () => {
      const t = currentTarget();
      if (!alive) return;
      setTarget((old) => (old?.appid === t?.appid && old?.running === t?.running ? old : t));
      try {
        const o = await getOverview(t?.appid ?? 0);
        if (alive) setOverview(o);
      } catch (e) {
        console.error("HDR Ket: overview failed", e);
      }
    };
    tick();
    const id = setInterval(tick, 1000);
    return () => {
      alive = false;
      clearInterval(id);
    };
  }, []);

  useEffect(() => {
    setEnabled(null);
    if (appid) getLaunchOptions(appid).then((o) => setEnabled(hasHdrKet(o)));
  }, [appid]);

  useEffect(() => {
    if (overview && !overview.display.configured && !firstRunShown) {
      firstRunShown = true;
      openPeakModal(true);
    }
  }, [overview?.display.configured]);

  if (!overview) {
    return (
      <PanelSection>
        <PanelSectionRow>
          <Field description="Loading…" />
        </PanelSectionRow>
      </PanelSection>
    );
  }

  const { display, game, layer } = overview;

  function openPeakModal(firstRun: boolean) {
    if (!overview) return;
    showModal(
      <PeakModal
        display={overview.display}
        firstRun={firstRun}
        onSave={async (nits) => setOverview(await setDefaultPeak(nits, appid))}
      />,
    );
  }

  const value = <K extends keyof Settings>(key: K): Settings[K] => {
    const p = pending.current[key];
    if (p && Date.now() - p.at < 2500) return p.value as Settings[K];
    return game!.settings[key];
  };

  const send = (changes: Record<string, Value>) => {
    const now = Date.now();
    for (const [k, v] of Object.entries(changes)) pending.current[k] = { value: v, at: now };
    Object.assign(queued.current, changes);
    clearTimeout(sendTimer.current);
    sendTimer.current = setTimeout(async () => {
      const batch = queued.current;
      queued.current = {};
      try {
        setOverview(await setSettings(appid, batch));
      } catch (e) {
        console.error("HDR Ket: update failed", e);
      }
    }, 120);
  };

  const toggleEnabled = async (on: boolean) => {
    const options = await getLaunchOptions(appid);
    setLaunchOptions(appid, on ? addHdrKet(options) : removeHdrKet(options));
    setEnabled(on);
  };

  const status = (() => {
    if (!target || !game) return "";
    if (game.live && enabled === false) return "Active now; turns off when the game restarts.";
    if (game.live) {
      const st = game.stats;
      if (!st?.frames) return "Active.";
      const parts = [`Peak on screen ${st.peak} nits`];
      if (value("tonemap") === "adaptive")
        parts.push(`scene white ${pct(st.scene_white)}%` + (st.shadow_lift > 0.02 ? `, shadows +${pct(st.shadow_lift)}%` : ""));
      if (st.wide_gamut > 0.001) parts.push(`${(st.wide_gamut * 100).toFixed(1)}% beyond sRGB`);
      return "Active. " + parts.join(" · ");
    }
    if (enabled && target.running) return "Restart the game to turn HDR on.";
    if (enabled) return "On. Starts with the game.";
    return "Off for this game.";
  })();

  const compare = game?.live ? (value("bypass") ? "sdr" : (value("split") ?? 0) > 0 ? "split" : "hdr") : "hdr";
  const showControls = !!game && (enabled || game.live || game.has_profile);

  return (
    <>
      {layer.state === "missing" && (
        <PanelSection title="Layer not installed">
          <PanelSectionRow>
            <Field description="The HDR Ket Vulkan layer could not be installed. Reinstall the plugin, or check the Decky logs." />
          </PanelSectionRow>
        </PanelSection>
      )}

      {target && game ? (
        <PanelSection title={target.name}>
          <PanelSectionRow>
            <ToggleField
              label="HDR Ket"
              description={status}
              checked={!!enabled}
              disabled={enabled === null}
              onChange={toggleEnabled}
            />
          </PanelSectionRow>

          {showControls && (
            <>
              <PanelSectionRow>
                <ToggleField
                  label="Use display's peak"
                  description={`${display.default_peak} nits on ${display.name}`}
                  checked={!value("peak_override")}
                  onChange={(useDisplay) => send({ peak_override: !useDisplay })}
                />
              </PanelSectionRow>
              {value("peak_override") && (
                <PanelSectionRow>
                  <SliderField
                    label="Peak for this game"
                    value={value("peak_nits")}
                    min={200}
                    max={4000}
                    step={10}
                    showValue
                    valueSuffix=" nits"
                    onChange={(v) => send({ peak_nits: v })}
                  />
                </PanelSectionRow>
              )}
              <PanelSectionRow>
                <SliderField
                  label="Middle grey"
                  description={`SDR white at ${Math.round(value("middle_grey_nits") / 0.18)} nits`}
                  value={value("middle_grey_nits")}
                  min={10}
                  max={80}
                  step={0.5}
                  showValue
                  valueSuffix=" nits"
                  onChange={(v) => send({ middle_grey_nits: v })}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField
                  label="Highlight expansion"
                  value={pct(value("expansion"))}
                  min={0}
                  max={150}
                  step={1}
                  showValue
                  valueSuffix="%"
                  onChange={(v) => send({ expansion: v / 100 })}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <DropdownItem
                  label="Tone mapping"
                  rgOptions={TONEMAP_OPTIONS}
                  selectedOption={value("tonemap")}
                  onChange={(o) => send({ tonemap: o.data })}
                />
              </PanelSectionRow>
              {value("tonemap") === "adaptive" && (
                <PanelSectionRow>
                  <SliderField
                    label="Adaptive strength"
                    value={pct(value("adapt_strength"))}
                    min={0}
                    max={100}
                    step={1}
                    showValue
                    valueSuffix="%"
                    onChange={(v) => send({ adapt_strength: v / 100 })}
                  />
                </PanelSectionRow>
              )}
              <PanelSectionRow>
                <DropdownItem
                  label="Content gamma"
                  rgOptions={EOTF_OPTIONS}
                  selectedOption={value("eotf")}
                  onChange={(o) => send({ eotf: o.data })}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField
                  label="Saturation"
                  value={pct(value("saturation"))}
                  min={0}
                  max={200}
                  step={1}
                  showValue
                  valueSuffix="%"
                  onChange={(v) => send({ saturation: v / 100 })}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField
                  label="Gamut expansion"
                  value={pct(value("gamut_expansion"))}
                  min={0}
                  max={100}
                  step={1}
                  showValue
                  valueSuffix="%"
                  onChange={(v) => send({ gamut_expansion: v / 100 })}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField
                  label="Black floor"
                  description="Raise if the game's blacks look grey"
                  value={Math.round(value("black_floor") * 1000)}
                  min={0}
                  max={100}
                  step={1}
                  showValue
                  onChange={(v) => send({ black_floor: v / 1000 })}
                />
              </PanelSectionRow>

              {game.live && (
                <>
                  <PanelSectionRow>
                    <DropdownItem
                      label="Compare"
                      rgOptions={COMPARE_OPTIONS}
                      selectedOption={compare}
                      onChange={(o) => send({ bypass: o.data === "sdr", split: o.data === "split" ? 0.5 : 0 })}
                    />
                  </PanelSectionRow>
                  <PanelSectionRow>
                    <ToggleField
                      label="Test pattern"
                      description="Bars from 50 to 4000 nits across the top of the screen"
                      checked={!!value("pattern")}
                      onChange={(on) => send({ pattern: on })}
                    />
                  </PanelSectionRow>
                </>
              )}

              <PanelSectionRow>
                <ToggleField label="Advanced" checked={advanced} onChange={setAdvanced} />
              </PanelSectionRow>
              {advanced && (
                <>
                  <PanelSectionRow>
                    <SliderField
                      label="Expansion starts at"
                      value={pct(value("shoulder_start"))}
                      min={0}
                      max={95}
                      step={1}
                      showValue
                      valueSuffix="%"
                      onChange={(v) => send({ shoulder_start: v / 100 })}
                    />
                  </PanelSectionRow>
                  <PanelSectionRow>
                    <SliderField
                      label="Expansion curve"
                      value={value("shoulder_pow")}
                      min={0.5}
                      max={8}
                      step={0.05}
                      showValue
                      onChange={(v) => send({ shoulder_pow: v })}
                    />
                  </PanelSectionRow>
                  <PanelSectionRow>
                    <SliderField
                      label="Adaptive release"
                      value={value("adapt_release")}
                      min={0.2}
                      max={8}
                      step={0.1}
                      showValue
                      valueSuffix=" s"
                      onChange={(v) => send({ adapt_release: v })}
                    />
                  </PanelSectionRow>
                </>
              )}

              <PanelSectionRow>
                <ButtonItem
                  layout="below"
                  onClick={() =>
                    showModal(
                      <ConfirmModal
                        strTitle="Reset to defaults?"
                        strDescription={`${target.name} goes back to the default settings and the display's peak.`}
                        strOKButtonText="Reset"
                        onOK={async () => {
                          pending.current = {};
                          setOverview(await resetGame(appid));
                        }}
                      />,
                    )
                  }
                >
                  Reset to defaults
                </ButtonItem>
              </PanelSectionRow>
              <PanelSectionRow>
                <Field
                  description={
                    game.live && game.url
                      ? `Saved automatically. Tune from your phone: ${game.url}`
                      : "Saved automatically for this game."
                  }
                />
              </PanelSectionRow>
            </>
          )}
        </PanelSection>
      ) : (
        <PanelSection title="Game">
          <PanelSectionRow>
            <Field description="Open a game's page in your library, or start a game, to turn HDR Ket on for it and tune its profile." />
          </PanelSectionRow>
        </PanelSection>
      )}

      <PanelSection title="Display">
        <PanelSectionRow>
          <Field
            label={display.name}
            description={
              display.count > 1 ? "Several displays are connected; settings apply to this one." : undefined
            }
          >
            {display.default_peak} nits
          </Field>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => openPeakModal(false)}>
            Change default peak
          </ButtonItem>
        </PanelSectionRow>
        {layer.state === "developer" && (
          <PanelSectionRow>
            <Field description={`Using a developer build of the layer: ${layer.path}`} />
          </PanelSectionRow>
        )}
      </PanelSection>
    </>
  );
}

export default definePlugin(() => ({
  name: "HDR Ket",
  titleView: <div className={staticClasses.Title}>HDR Ket</div>,
  content: <Content />,
  icon: <MdHdrOn />,
}));
