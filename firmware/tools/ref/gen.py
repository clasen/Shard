"""Writes controlled Obsidian test patches (zz NN ...) from a factory template, one module per patch.

Usage: gen.py <out dir>   (one <name>.obs/Package.obs per test)
Copy them into NS2's Documents/Instruments/Obsidian/User and run Manage -> Refresh there; build the port's
side with content2bin.py on the same folder.
"""
import os, re, sys
import xml.etree.ElementTree as ET

TEMPLATE = "/Applications/NanoStudio 2.app/Wrapper/NanoStudio 2.app/Factory/Instruments/Obsidian/Deep Bass.obs/Package.obs"


def base():
    r = ET.parse(TEMPLATE).getroot()
    v = r.find("Voice")
    v.set("Poly", "16"); v.set("Uni", "1"); v.set("UDtn", "0"); v.set("USte", "0"); v.set("GTim", "0")
    o = v.find("Osc"); o.set("Type", "Layer")
    for i in range(3):
        oc = o.find(f"Osc{i}")
        oc.set("En", "On" if i == 0 else "Off"); oc.set("Type", "Analog"); oc.set("Trn", "0"); oc.set("Dtn", "0")
        oc.set("Lev", "1"); oc.set("Pan", "0.5"); oc.set("FMix", "0"); oc.set("Ring", "0"); oc.set("POff", "0")
        oc.find("Anlg").set("Wave", "Saw"); oc.find("Anlg").set("Sync", "Off")
    f = v.find("Filt"); f.set("Rout", "PARALLEL"); f.set("O1BP", "OFF")
    for k in ("F1", "F2", "F3"):
        e = f.find(k); e.set("En", "Off"); e.set("Type", "LP 24-A"); e.set("CutO", "1"); e.set("Q", "0"); e.set("Drv", "0")
        e.set("KeyT", "0"); e.set("OGn", "1"); e.set("Pan", "0.5"); e.set("SGan", "0")
    for e in v.find("Env"):
        for a, val in dict(A="0", D="0.3", Brk="0", S="1", D2="0", R="0.1", Levl="1", Dly="0", SLv="0", RLv="0",
                           AC="2", SC="1.22051", DC="1.22051", RC="1.22051", VTim="0", VLev="0", KTim="0", KLev="0").items():
            e.set(a, val)
        e.set("Type", "Bipolar" if e.tag == "EnvP" else "ADSR")
    for c in v.find("Mod"):
        if re.fullmatch(r"C\d\d", c.tag):
            c.set("Src", "None"); c.set("Dest", "None"); c.set("DstP", "---"); c.set("Sca", "None"); c.set("Dept", "100")
    for l in v.find("LFO"):
        l.set("Ampl", "0")
    for fx in r.iter():
        if "Mix" in fx.attrib and fx.tag not in ("NS",):
            fx.set("Mix", "0")
    return r


def F1(r, **kw):
    e = r.find("Voice/Filt/F1"); e.set("En", "On")
    for k, val in kw.items(): e.set(k, str(val))


def env(r, tag, **kw):
    e = r.find(f"Voice/Env/{tag}")
    for k, val in kw.items(): e.set(k, str(val))


def conn(r, slot, src, dest, par, dept):
    c = r.find(f"Voice/Mod/C{slot:02d}"); c.set("Src", src); c.set("Dest", dest); c.set("DstP", par); c.set("Dept", str(dept))


def fm(r, ratios=(1, 1, 1), **amounts):
    """Osc 1 as an FM oscillator with sine operators at the given ratios and connection amounts (O11..O3O)."""
    o = r.find("Voice/Osc/Osc0"); o.set("Type", "FM")
    f = o.find("FM")
    for op, rat in zip(f.find("OP"), ratios):
        op.set("Wave", "Sine"); op.set("FrqO", "0"); op.set("Rat", str(int(rat * 10000)))
    for c in f.find("Con"):
        c.set("Amt", str(amounts.get(c.tag, 0))); c.find("Env").set("Levl", "0")


def preset(name, **changes):
    """A factory preset with changes: f2_off/f3_off disable those filters, clear=[slots] empties mod slots, uni=n,
    fx_off turns the effects chain off."""
    r = ET.parse(os.path.join(os.path.dirname(TEMPLATE), "..", name + ".obs", "Package.obs")).getroot()
    v = r.find("Voice")
    for k in ("F2", "F3"):
        if changes.get(k.lower() + "_off"): v.find(f"Filt/{k}").set("En", "Off")
    for slot in changes.get("clear", ()):
        c = v.find(f"Mod/C{slot:02d}"); c.set("Src", "None"); c.set("Dest", "None"); c.set("DstP", "---")
    if "uni" in changes: v.set("Uni", str(changes["uni"]))
    if changes.get("fx_off"): r.find("FX").set("En", "0")
    return r


def tests():
    t = {}
    r = base(); t["zz01 Saw"] = r
    r = base(); r.find("Voice/Osc/Osc0/Anlg").set("Wave", "Square"); t["zz02 Square"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.5); t["zz03 LP24 c50"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.5, Q=0.7); t["zz04 LP24 c50 q70"] = r
    r = base(); F1(r, Type="LP 12-A", CutO=0.4, Drv=0.6); t["zz05 LP12 c40 d60"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.15); env(r, "EnvF", D=0.6, S=0.2, Levl=1); conn(r, 0, "Filter Env", "Filter 1", "Cutoff", 60); t["zz06 FiltEnv"] = r
    r = base(); env(r, "EnvA", A=0.2, D=0.5, S=0.4, R=0.6); t["zz07 AmpEnv"] = r
    r = base(); env(r, "EnvA", VLev=1); t["zz08 AmpVel"] = r
    r = base(); F1(r, Type="HP 24-A", CutO=0.6, Q=0.4); t["zz09 HP24 c60 q40"] = r
    r = base(); F1(r, Type="LP 12-D", CutO=0.5, Q=0.5); t["zz10 LP12D c50 q50"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.7, Drv=1); t["zz11 Drv100 L100"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.7, Drv=1); r.find("Voice/Osc/Osc0").set("Lev", "0.25"); t["zz12 Drv100 L25"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.7, Drv=0.3); t["zz13 Drv30 L100"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.7, Drv=0.6); r.find("Voice/Osc/Osc0").set("Lev", "0.25"); t["zz14 Drv60 L25"] = r
    r = base(); env(r, "EnvA", Levl=0.5); t["zz15 AmpLevl50"] = r
    r = base(); r.find("Voice/Osc/Osc0/Anlg").set("Wave", "Sine"); t["zz16 Sine"] = r
    r = base(); r.find("Voice/Osc/Osc0/Anlg").set("Wave", "Tri"); t["zz17 Tri"] = r
    r = base(); conn(r, 0, "Knob 1", "Amp Env", "Attack", 50); r.find("Autm").set("C02", "1"); t["zz18 Knob1Atk Autm"] = r
    r = base(); conn(r, 0, "Knob 1", "Osc 1", "Level", -50); r.find("Autm").set("C02", "1"); t["zz19 Knob1Lev Autm"] = r
    r = base(); fm(r, O3O=1); t["zz20 FM carrier"] = r
    r = base(); fm(r, O23=0.5, O3O=1); t["zz21 FM 2to3"] = r
    r = base(); fm(r, (2, 1, 1), O13=0.3, O3O=1); t["zz22 FM 1to3 r2"] = r
    r = base(); fm(r, O22=0.3, O2O=1); t["zz23 FM fb2"] = r
    r = base(); F1(r, Type="EQ Shelf", CutO=0.164, SGan=24, OGn=0.377); t["zz24 EQShelf"] = r
    r = base(); F1(r, Type="EQ Peak", CutO=0.5, SGan=12, Q=0); t["zz25 EQPeak"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=1, OGn=0.4); t["zz26 OGn40"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.6); r.find("Voice/Filt").set("Rout", "SERIES")
    e = r.find("Voice/Filt/F2"); e.set("En", "On"); e.set("Type", "LP 24-A"); e.set("CutO", "0.6"); t["zz27 Series"] = r
    r = base(); F1(r, Type="EQ Shelf", CutO=0.164, SGan=12); t["zz28 Shelf p12"] = r
    r = base(); F1(r, Type="EQ Shelf", CutO=0.164, SGan=-12); t["zz29 Shelf m12"] = r
    r = base(); F1(r, Type="EQ Shelf", CutO=0.5, SGan=24, Q=1); t["zz30 Shelf p24 q1"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.2); conn(r, 0, "Pad X", "Filter 1", "Cutoff Slow", 100)
    for k in range(1, 12): r.find("Autm").set(f"C{k:02d}", "0")
    t["zz31 PadX Autm0"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.2); conn(r, 0, "Pad X", "Filter 1", "Cutoff Slow", 100)
    for k in range(1, 12): r.find("Autm").set(f"C{k:02d}", "0")
    r.find("Autm").set("C10", "1"); t["zz32 PadX Autm10"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0, KeyT=1); t["zz33 LP24 c0 kt1"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.05, KeyT=1); t["zz34 LP24 c05 kt1"] = r
    r = base(); F1(r, Type="LP 24-A", CutO=0.1); t["zz35 LP24 c10"] = r
    r = base(); r.find("Voice/Osc/Osc0/Anlg").set("Wave", "Bi-Pulse"); r.find("Voice/Osc/Osc0/Anlg").set("PWM", "0.23"); t["zz36 BiPulse"] = r
    r = base(); r.find("Voice/Osc/Osc0/Anlg").set("Wave", "Pulse"); r.find("Voice/Osc/Osc0/Anlg").set("PWM", "0.23"); t["zz37 Pulse"] = r
    r = base(); a = r.find("Voice/Osc/Osc0/Anlg"); a.set("Sync", "ANALOG"); a.set("SRat", "0.5"); t["zz38 Sync"] = r
    r = base(); r.find("Voice/Osc/Osc0").set("Pan", "0.25"); t["zz39 OscPan"] = r
    r = base(); o1 = r.find("Voice/Osc/Osc1"); o1.set("En", "On"); o1.set("POff", "0.5"); r.find("Voice/Osc/Osc0").set("POff", "0"); t["zz40 POff"] = r
    r = base(); v = r.find("Voice"); v.set("Uni", "2"); v.set("UDtn", "0.3"); v.set("USte", "0"); t["zz41 Uni2"] = r
    r = base(); r.find("Voice/Osc/Osc0").set("Lev", "2"); t["zy01 Lev2"] = r
    r = base(); F1(r, Type="WS Tube", Drv=0.5, WSGn=0.5); t["zy02 F1 WSTube"] = r
    r = base(); e = r.find("Voice/Filt/F3"); e.set("En", "On"); e.set("Type", "WS Soft"); e.set("Drv", "0.3"); t["zy03 F3 WSSoft"] = r
    for tag, lev in (("zy04 FMenv L0", 0), ("zy05 FMenv L1", 1), ("zy06 FMenv L05", 0.5)):
        r = base(); fm(r, O3O=1)
        e = r.find("Voice/Osc/Osc0/FM/Con/O3O/Env"); e.set("D", "0.6"); e.set("S", "0"); e.set("R", "0.3"); e.set("Levl", str(lev))
        t[tag] = r
    r = base(); l = r.find("Voice/LFO/LFO1")
    for k, val in dict(Type="Square", RtBt="1", BRt="4", Sync="KEY", OutT="BIPOLAR", Ampl="1").items(): l.set(k, val)
    conn(r, 0, "LFO 1", "Amp", "Gain", -50); t["zy07 LFO tempo 1-4"] = r
    r = base(); v = r.find("Voice"); v.set("Uni", "4"); v.set("UDtn", "0.3"); v.set("USte", "0"); t["zy08 Uni4"] = r
    r = base(); o = r.find("Voice/Osc/Osc1"); o.set("En", "On"); o.set("Trn", "12"); F1(r, Type="LP 24-A", CutO=0.7, Drv=1)
    t["zy09 2osc Drv100"] = r
    for tag, typ, gn, drv in (("zw01 Tube g25 d0", "WS Tube", 0.25, 0), ("zw02 Tube g50 d0", "WS Tube", 0.5, 0),
                              ("zw03 Tube g100 d0", "WS Tube", 1, 0), ("zw04 Tube g50 d100", "WS Tube", 0.5, 1),
                              ("zw05 Soft g50 d0", "WS Soft", 0.5, 0), ("zw06 Soft g100 d50", "WS Soft", 1, 0.5),
                              ("zw07 Hard g50 d50", "WS Hard", 0.5, 0.5), ("zw08 Sine g50 d50", "WS Sine", 0.5, 0.5)):
        r = base(); F1(r, Type=typ, WSGn=gn, Drv=drv); t[tag] = r
    r = base(); r.find("Voice/Osc/Osc0").set("Lev", "0.25"); F1(r, Type="WS Tube", WSGn=0.5, Drv=0.5); t["zw09 Tube g50 d50 L25"] = r
    def f3(r, **kw):
        e = r.find("Voice/Filt/F3"); e.set("En", "On")
        for k, val in kw.items(): e.set(k, str(val))
    f3_ = f3
    for tag, typ, drv, lev, ogn in (("zu01 F3 Soft d30 L25", "WS Soft", 0.3, 0.25, 1), ("zu02 F3 Soft d30 L06", "WS Soft", 0.3, 0.0625, 1),
                                    ("zu03 F3 Soft d0", "WS Soft", 0, 1, 1), ("zu04 F3 Soft d100", "WS Soft", 1, 1, 1),
                                    ("zu05 F3 Tube d0", "WS Tube", 0, 1, 1), ("zu06 F3 Tube d100", "WS Tube", 1, 1, 1),
                                    ("zu07 F3 Soft d30 OGn50", "WS Soft", 0.3, 1, 0.5)):
        r = base(); r.find("Voice/Osc/Osc0").set("Lev", str(lev)); f3(r, Type=typ, Drv=drv, OGn=ogn); t[tag] = r
    r = base(); F1(r, Type="WS Tri", WSGn=0.5, Drv=0); t["zu08 F1 Tri d0"] = r
    r = base(); f3(r, Type="WS Soft", Drv=0.3); env(r, "EnvA", Levl=0.5); t["zu09 F3 Soft d30 Amp50"] = r
    for tag, cut in (("zq1 F3 Soft d30 c0", 0), ("zq2 F3 Soft d30 c25", 0.25), ("zq3 F3 Soft d30 c50", 0.5), ("zq4 F3 Soft d30 c75", 0.75)):
        r = base(); f3(r, Type="WS Soft", Drv=0.3, CutO=cut); t[tag] = r
    for tag, cut in (("zq5 F1 Tube d50 c0", 0), ("zq6 F1 Tube d50 c50", 0.5)):
        r = base(); F1(r, Type="WS Tube", Drv=0.5, CutO=cut); t[tag] = r
    r = base(); F1(r, Type="WS Tube", Drv=0.5, CutO=0, KeyT=1); t["zq7 F1 Tube d50 c0 kt1"] = r
    for tag, name in (("zt01 AK WSGn50", "Analogue Kick"), ("zt03 CA WSGn50", "Classic Acid")):
        r = preset(name); r.find("Voice/Filt/F3").set("WSGn", "0.5"); t[tag] = r
    t["zt02 AK noF3"] = preset("Analogue Kick", f3_off=1)
    t["zt04 CA noF3"] = preset("Classic Acid", f3_off=1)
    # Effects: zf01..zf18 are for the short-note sequence (rec.sh <out> fxseq, render ... fx).
    def unit(r, tag, **kw):
        e = next(r.iter(tag)); e.set("En", "1")
        for k, val in kw.items(): e.set(k, str(val))
    dly = dict(Type="Stereo", DTim=0.25, TNum=1, TDen="sec", EQCt=0.5, FB=0.5, Mix=0.5)
    for tag, kw in (("zf01 Dry", None), ("zf02 Dly Stereo", {}), ("zf03 Dly Cross12", dict(Type="Cross 1:2")),
                    ("zf04 Dly Pingpong12", dict(Type="Pingpong 1:2")), ("zf05 Dly EQ10", dict(EQCt=0.1)),
                    ("zf06 Dly EQ90", dict(EQCt=0.9)), ("zf07 Dly Mix100 FB0", dict(Mix=1, FB=0)),
                    ("zf08 Dly 1-8 tempo", dict(TDen="8", FB=0.3))):
        r = base()
        if kw is not None: unit(r, "Dly", **{**dly, **kw})
        t[tag] = r
    rev = dict(Type="Plate 1", LCut=0.25, HCut=0.25, Decy=0.5, HDmp=0.25, Mix=0.5)
    for tag, kw in (("zf09 Rev Plate1", {}), ("zf10 Rev Plate1 Mix100", dict(Mix=1)), ("zf11 Rev Plate1 Decy10", dict(Decy=0.1)),
                    ("zf12 Rev Plate1 Decy90", dict(Decy=0.9)), ("zf13 Rev Plate1 HDmp80", dict(HDmp=0.8)),
                    ("zf14 Rev Plate1 Cuts80", dict(LCut=0.8, HCut=0.8)), ("zf15 Rev SmallRoom1", dict(Type="Small Room 1")),
                    ("zf16 Rev LargeRoom1", dict(Type="Large Room 1")), ("zf17 Rev DenseRoom1", dict(Type="Dense Room 1")),
                    ("zf18 Rev Hall", dict(Type="Hall"))):
        r = base(); unit(r, "Rev", **{**rev, **kw}); t[tag] = r
    for tag, kw in (("zf28 Rev SmallRoom1 Decy10", dict(Type="Small Room 1", Decy=0.1)), ("zf29 Rev SmallRoom1 Decy90", dict(Type="Small Room 1", Decy=0.9)),
                    ("zf30 Rev LargeRoom1 Decy10", dict(Type="Large Room 1", Decy=0.1)), ("zf31 Rev LargeRoom1 Decy90", dict(Type="Large Room 1", Decy=0.9)),
                    ("zf32 Rev DenseRoom1 Decy10", dict(Type="Dense Room 1", Decy=0.1)), ("zf33 Rev DenseRoom1 Decy90", dict(Type="Dense Room 1", Decy=0.9)),
                    ("zf34 Rev SmallRoom1 HDmp80", dict(Type="Small Room 1", HDmp=0.8)), ("zf35 Rev Plate2", dict(Type="Plate 2")),
                    ("zf36 Rev SmallRoom2", dict(Type="Small Room 2"))):
        r = base(); unit(r, "Rev", **{**rev, **kw}); t[tag] = r
    mfx = dict(Rat=0.25, Dep=0.5, Dly=0.5, FB=0, Wid=0.25, Mix=1)
    for tag, kw in (("zf19 Chorus1", dict(Type="Chorus 1")), ("zf20 Chorus2", dict(Type="Chorus 2")),
                    ("zf21 Ensemble1", dict(Type="Ensemble 1")), ("zf22 Ensemble2", dict(Type="Ensemble 2")),
                    ("zf23 Flanger1", dict(Type="Flanger 1", FB=0.5)), ("zf24 Flanger2", dict(Type="Flanger 2", FB=0.5)),
                    ("zf25 Phaser1", dict(Type="Phaser 1", FB=0.5)), ("zf26 Phaser2", dict(Type="Phaser 2", FB=0.5)),
                    ("zf27 Chorus1 Mix50", dict(Type="Chorus 1", Mix=0.5))):
        r = base(); unit(r, "MFX", **{**mfx, **kw}); t[tag] = r
    for tag, kw in (("zf40 Chorus1 Rat75", dict(Type="Chorus 1", Rat=0.75)), ("zf41 Chorus1 Dep100", dict(Type="Chorus 1", Dep=1)),
                    ("zf42 Chorus1 Dly10", dict(Type="Chorus 1", Dly=0.1)), ("zf43 Chorus1 Dly90", dict(Type="Chorus 1", Dly=0.9)),
                    ("zf44 Flanger1 Dly10", dict(Type="Flanger 1", Dly=0.1, FB=0.5)), ("zf45 Flanger1 Dly90", dict(Type="Flanger 1", Dly=0.9, FB=0.5)),
                    ("zf46 Flanger1 FB0", dict(Type="Flanger 1")), ("zf47 Chorus2 Rat5", dict(Type="Chorus 2", Rat=0.05)),
                    ("zf48 Ensemble1 Rat5", dict(Type="Ensemble 1", Rat=0.05))):
        r = base(); unit(r, "MFX", **{**mfx, **kw}); t[tag] = r
    # Mix destinations driven by Knob 1 (Autm C04 = 1), for the short-note sequence.
    r = base(); unit(r, "Rev", **{**rev, "Mix": 0}); conn(r, 0, "Knob 1", "FX", "Reverb Mix", 50); r.find("Autm").set("C04", "1")
    t["zf49 Rev Plate1 KnobMix50"] = r
    r = base(); unit(r, "Dly", **{**dly, "Mix": 0}); conn(r, 0, "Knob 1", "FX", "Delay Mix", 50); r.find("Autm").set("C04", "1")
    t["zf50 Dly Stereo KnobMix50"] = r
    def osc(r, typ, sub=None, **kw):
        o = r.find("Voice/Osc/Osc0"); o.set("Type", typ)
        if sub:
            e = o.find(sub)
            for k, val in kw.items(): e.set(k, str(val))
    ns = dict(Mix=0.75, FlOf=0, Dtn=0.25, Sprd=0.75)
    for tag, kw in (("zn01 NS Mix0", dict(Mix=0)), ("zn02 NS Mix50", dict(Mix=0.5)), ("zn03 NS Mix100", dict(Mix=1)),
                    ("zn04 NS Mix100 Dtn75", dict(Mix=1, Dtn=0.75)), ("zn05 NS Mix100 FlOf100", dict(Mix=1, FlOf=1))):
        r = base(); osc(r, "NanoSaw", "NS", **{**ns, **kw}); t[tag] = r
    r = base(); osc(r, "PD", "PD", Wave="Saw", PD=0.5); t["zn06 PD Saw50"] = r
    r = base(); osc(r, "PD", "PD", Wave="Square", PD=0.5); t["zn07 PD Square50"] = r
    r = base(); osc(r, "Noise", "Nois", Type="White", Deci=0); t["zn08 Noise White"] = r
    r = base(); osc(r, "Analog", "Anlg", Wave="Saw", Sync="ANALOG", SRat=0.25, SShp=1); t["zn09 Sync SRat25"] = r
    for tag, kw, f3 in (("zm01 FormM v0", dict(Type="Formant-M", FVwl=0), 0), ("zm02 FormM v50", dict(Type="Formant-M", FVwl=0.5), 0),
                        ("zm03 FormM v100", dict(Type="Formant-M", FVwl=1), 0), ("zm04 FormM v50 Q50", dict(Type="Formant-M", FVwl=0.5, Q=0.5), 0),
                        ("zm05 FormM v50 Q100", dict(Type="Formant-M", FVwl=0.5, Q=1), 0), ("zm06 FormF v50", dict(Type="Formant-F", FVwl=0.5), 0),
                        ("zm07 F3 FormM v50", dict(Type="Formant-M", FVwl=0.5), 1), ("zm08 FormM v50 c20", dict(Type="Formant-M", FVwl=0.5, CutO=0.2), 0),
                        ("zm09 FormF v0", dict(Type="Formant-F", FVwl=0), 0)):
        r = base(); osc(r, "Noise", "Nois", Type="White", Deci=0)
        if f3: f3_(r, **kw)
        else: F1(r, **kw)
        t[tag] = r
    for tag, kw in (("zm10 FormM c50 v0", dict(Type="Formant-M", CutO=0.5, FVwl=0, Q=0.5)),
                    ("zm11 FormM c50 v50", dict(Type="Formant-M", CutO=0.5, FVwl=0.5, Q=0.5)),
                    ("zm12 FormM c50 v100", dict(Type="Formant-M", CutO=0.5, FVwl=1, Q=0.5)),
                    ("zm13 FormF c50 v50", dict(Type="Formant-F", CutO=0.5, FVwl=0.5, Q=0.5)),
                    ("zm14 FormM c30 v50", dict(Type="Formant-M", CutO=0.3, FVwl=0.5, Q=0.5)),
                    ("zm15 FormM c70 v50", dict(Type="Formant-M", CutO=0.7, FVwl=0.5, Q=0.5)),
                    ("zm16 FormM c50 v50 Q0", dict(Type="Formant-M", CutO=0.5, FVwl=0.5, Q=0))):
        r = base(); osc(r, "Noise", "Nois", Type="White", Deci=0); F1(r, **kw); t[tag] = r
    for tag, name, kw in (("zx01 FatWide core", "Fat Wide Pad", dict(fx_off=1, uni=1)), ("zx02 FatWide noFX", "Fat Wide Pad", dict(fx_off=1)),
                          ("zx03 IronDrops noFX", "Iron Drops", dict(fx_off=1)), ("zx04 Dubstep core", "Dubstep Crush", dict(fx_off=1, uni=1)),
                          ("zx05 Dubstep noFX", "Dubstep Crush", dict(fx_off=1)), ("zx06 PluckPad core", "Pluck or Pad", dict(fx_off=1, uni=1)),
                          ("zx07 PluckPad noFX", "Pluck or Pad", dict(fx_off=1)), ("zx08 EP2 noFX", "Electric Piano 2", dict(fx_off=1)),
                          ("zx09 7thHouse noFX", "7th House", dict(fx_off=1))):
        t[tag] = preset(name, **kw)
    for tag, pan in (("zp01 OscPan0", 0), ("zp02 OscPan100", 1), ("zp03 OscPan75", 0.75)):
        r = base(); r.find("Voice/Osc/Osc0").set("Pan", str(pan)); t[tag] = r
    for tag, uni, ste in (("zp04 Uni2 Ste100", 2, 1), ("zp05 Uni2 Ste50", 2, 0.5), ("zp06 Uni4 Ste100", 4, 1)):
        r = base(); v = r.find("Voice"); v.set("Uni", str(uni)); v.set("UDtn", "0.3"); v.set("USte", str(ste)); t[tag] = r
    def fmw(r, waves):
        for op, w in zip(r.find("Voice/Osc/Osc0/FM/OP"), waves): op.set("Wave", w)
    r = base(); fm(r, O3O=0.5); t["zk01 FM O3O50"] = r
    r = base(); fm(r, O3O=0.25); t["zk02 FM O3O25"] = r
    r = base(); fm(r, O3O=1); fmw(r, ("Sine", "Sine", "Saw Down")); t["zk03 FM SawDown out"] = r
    r = base(); fm(r, O3O=1); fmw(r, ("Sine", "Sine", "Saw Up")); t["zk04 FM SawUp out"] = r
    r = base(); fm(r, (1, 2, 1), O2O=0.5, O3O=0.5); t["zk05 FM O2O50 O3O50"] = r
    r = base(); fm(r, O11=0.5, O1O=1); fmw(r, ("Saw Down", "Sine", "Sine")); t["zk06 FM SawDown fb50"] = r
    r = base(); fm(r, O12=0.5, O2O=1); t["zk07 FM 1to2 50"] = r
    def solo(name, keep):
        r = preset(name, fx_off=1, uni=1)
        for i in range(3):
            if i != keep: r.find(f"Voice/Osc/Osc{i}").set("En", "Off")
        return r
    t["zk08 FatWide osc1"] = solo("Fat Wide Pad", 0)
    t["zk09 EP2 osc1"] = solo("Electric Piano 2", 0)
    t["zd1 DeepBass noF23"] = preset("Deep Bass", f2_off=1, f3_off=1)
    t["zd2 DeepBass noLFO"] = preset("Deep Bass", clear=(3, 4, 5))
    t["zd3 DeepBass uni1"] = preset("Deep Bass", uni=1)
    t["zd4 DeepBass F1env"] = preset("Deep Bass", f2_off=1, f3_off=1, clear=range(1, 24), uni=1)
    t["zd5 DeepBass noEnv5WT"] = preset("Deep Bass", uni=1, clear=(2,))
    t["zd7 DeepBass C01-06"] = preset("Deep Bass", f2_off=1, f3_off=1, clear=range(7, 24), uni=1)
    t["zd8 DeepBass C07-12"] = preset("Deep Bass", f2_off=1, f3_off=1, clear=list(range(1, 7)) + list(range(13, 24)), uni=1)
    t["zd6 DeepBass F1env Env5WT"] = preset("Deep Bass", f2_off=1, f3_off=1, clear=[1] + list(range(3, 24)), uni=1)
    return t


if __name__ == "__main__":
    out = sys.argv[1]
    for name, r in tests().items():
        d = os.path.join(out, name + ".obs"); os.makedirs(d, exist_ok=True)
        ET.ElementTree(r).write(os.path.join(d, "Package.obs"), xml_declaration=True, encoding="unicode")
        print(name)
