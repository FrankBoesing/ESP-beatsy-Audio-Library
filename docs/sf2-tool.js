(function () {
  "use strict";

  var fileInput = document.getElementById("sf2File");
  var presetSelect = document.getElementById("sf2Preset");
  var velocityInput = document.getElementById("sf2Velocity");
  var noteInput = document.getElementById("sf2PreviewNote");
  var nameInput = document.getElementById("sf2Name");
  var playButton = document.getElementById("sf2Play");
  var stopButton = document.getElementById("sf2Stop");
  var exportButton = document.getElementById("sf2Export");
  var copyButton = document.getElementById("sf2Copy");
  var downloadButton = document.getElementById("sf2Download");
  var codePanel = document.getElementById("sf2CodePanel");
  var codeOutput = document.getElementById("sf2Code");
  var status = document.getElementById("sf2Status");
  var summary = document.getElementById("sf2Summary");
  var metadata = document.getElementById("sf2Metadata");
  var presetInfo = document.getElementById("sf2PresetInfo");
  var analyzerPanel = document.getElementById("sf2Analyzer");
  var allPresetScanButton = document.getElementById("sf2ScanAllPresets");
  var allPresetScanStatus = document.getElementById("sf2ScanStatus");
  var allPresetScanPanel = document.getElementById("sf2ScanPanel");
  var allPresetScanLog = document.getElementById("sf2ScanLog");
  var allPresetScanCopyButton = document.getElementById("sf2ScanCopy");
  var allPresetScanDownloadButton = document.getElementById("sf2ScanDownload");
  var outputInfo = document.getElementById("sf2OutputInfo");
  var wavetableModeInput = document.getElementById("sf2WavetableMode");
  var exportVersionSelect = document.getElementById("sf2ExportVersion");
  var keyboardStartSelect = document.getElementById("sf2KeyboardStart");
  var keyboardElement = document.getElementById("sf2Keyboard");
  var previewStatus = document.getElementById("sf2PreviewStatus");
  var modeInfo = document.getElementById("sf2ModeInfo");
  var keyboardLayoutInfo = document.getElementById("sf2KeyboardLayout");

  if (!fileInput || !presetSelect || !velocityInput || !exportButton) return;

  var font = null;
  var currentFileName = "";
  var loadGeneration = 0;
  var allPresetScanGeneration = 0;
  var audioContext = null;
  var masterGain = null;
  var activeVoices = [];
  var keyboardHolds = Object.create(null);
  var pointerHolds = Object.create(null);
  var pcHolds = Object.create(null);
  var pcNoteByCode = Object.create(null);
  var keyboardLayout = "unknown";
  var playbackGeneration = 0;
  var generatedCode = "";
  var generatedFileName = "";
  var presetCompatibilityMessage = "";
  
  var GEN = {
    startOffset: 0, endOffset: 1, startLoopOffset: 2, endLoopOffset: 3,
    startCoarse: 4, delayModLfo: 21, delayVibLfo: 23, freqVibLfo: 24,
    delayModEnv: 25, attackModEnv: 26, holdModEnv: 27, decayModEnv: 28,
    sustainModEnv: 29, releaseModEnv: 30,
    delayVolEnv: 33, attackVolEnv: 34, holdVolEnv: 35, decayVolEnv: 36,
    sustainVolEnv: 37, releaseVolEnv: 38, keyToVolHold: 39, keyToVolDecay: 40,
    instrument: 41, keyRange: 43, velocityRange: 44, startLoopCoarse: 45,
    keynum: 46, velocity: 47, attenuation: 48, endLoopCoarse: 50,
    coarseTune: 51, fineTune: 52, sampleId: 53, sampleModes: 54,
    scaleTuning: 56, rootKey: 58, endCoarse: 12
  };

  var DEFAULTS = {};
  DEFAULTS[GEN.delayVolEnv] = -12000;
  DEFAULTS[GEN.attackVolEnv] = -12000;
  DEFAULTS[GEN.holdVolEnv] = -12000;
  DEFAULTS[GEN.decayVolEnv] = -12000;
  DEFAULTS[GEN.sustainVolEnv] = 0;
  DEFAULTS[GEN.releaseVolEnv] = -12000;
  DEFAULTS[GEN.keyToVolHold] = 0;
  DEFAULTS[GEN.keyToVolDecay] = 0;
  DEFAULTS[GEN.attenuation] = 0;
  DEFAULTS[GEN.coarseTune] = 0;
  DEFAULTS[GEN.fineTune] = 0;
  DEFAULTS[GEN.sampleModes] = 0;
  DEFAULTS[GEN.scaleTuning] = 100;
  DEFAULTS[GEN.rootKey] = -1;
  DEFAULTS[GEN.keynum] = -1;
  DEFAULTS[GEN.velocity] = -1;
  DEFAULTS[GEN.startOffset] = 0;
  DEFAULTS[GEN.endOffset] = 0;
  DEFAULTS[GEN.startLoopOffset] = 0;
  DEFAULTS[GEN.endLoopOffset] = 0;
  DEFAULTS[GEN.startCoarse] = 0;
  DEFAULTS[GEN.endCoarse] = 0;
  DEFAULTS[GEN.startLoopCoarse] = 0;
  DEFAULTS[GEN.endLoopCoarse] = 0;

  var V1_INDEX_BITS = 16;
  var V2_PHASE_FRACTION_BITS = 14;
  var V2_PHASE_SCALE = Math.pow(2, V2_PHASE_FRACTION_BITS);
  var MAX_SAMPLE_FRAMES_V1 = Math.pow(2, V1_INDEX_BITS) - 1;
  var MAX_SAMPLE_FRAMES_V2 = Math.pow(2, 32 - V2_PHASE_FRACTION_BITS) - 1;
  var MAX_FILE_BYTES = 512 * 1024 * 1024;
  var MAX_EXPORT_FRAMES = 2000000;
  var RANGE_OPERATORS = [GEN.keyRange, GEN.velocityRange];
  var INDEX_OPERATORS = [GEN.instrument, GEN.sampleId];
  var UNSIGNED_OPERATORS = [GEN.instrument, GEN.sampleId, GEN.sampleModes];
  var PRESET_IGNORED_OPERATORS = [
    0, 1, 2, 3, 4, 12, 45, 46, 47, 50, 54, 57, 58
  ];
  var SUPPORTED_OPERATORS = [
    0, 1, 2, 3, 4, 12, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 43, 44, 45, 46, 47, 48, 50, 51, 52, 53, 54, 56, 58
  ];

  function setStatus(message, kind) {
    status.className = "status" + (kind ? " " + kind : "");
    status.textContent = message;
  }

  function setPreviewStatus(message) {
    if (!previewStatus) return;
    previewStatus.textContent = message || "";
  }

  function clearPreviewStatus() {
    setPreviewStatus("");
  }

  function fourCC(view, offset) {
    return String.fromCharCode(
      view.getUint8(offset), view.getUint8(offset + 1),
      view.getUint8(offset + 2), view.getUint8(offset + 3)
    );
  }

  function readChunks(view, start, end) {
    var chunks = [];
    var offset = start;
    while (offset + 8 <= end) {
      var id = fourCC(view, offset);
      var size = view.getUint32(offset + 4, true);
      var data = offset + 8;
      var next = data + size + (size & 1);
      if (next > end) throw new Error("RIFF chunk " + id + " exceeds its parent.");
      chunks.push({ id: id, data: data, size: size });
      offset = next;
    }
    if (offset !== end) throw new Error("Invalid RIFF chunk alignment.");
    return chunks;
  }

  function findChunk(chunks, id) {
    for (var i = 0; i < chunks.length; i++) {
      if (chunks[i].id === id) return chunks[i];
    }
    return null;
  }

  function findList(view, chunks, listType) {
    for (var i = 0; i < chunks.length; i++) {
      var chunk = chunks[i];
      if (chunk.id === "LIST" && chunk.size >= 4 &&
          fourCC(view, chunk.data) === listType) {
        return readChunks(view, chunk.data + 4, chunk.data + chunk.size);
      }
    }
    return null;
  }

  function readName(view, offset, length) {
    var result = "";
    for (var i = 0; i < length; i++) {
      var ch = view.getUint8(offset + i);
      if (ch === 0) break;
      result += String.fromCharCode(ch);
    }
    return result.replace(/[\u0000-\u001f\u007f]/g, "").trim();
  }

  function readText(view, chunk) {
    var result = "";
    if (!chunk) return result;
    for (var i = 0; i < chunk.size; i++) {
      var ch = view.getUint8(chunk.data + i);
      if (ch === 0) break;
      result += String.fromCharCode(ch);
    }
    return result.trim();
  }

  function records(view, chunk, recordSize, label, reader) {
    if (!chunk) throw new Error("Missing required SF2 chunk: " + label + ".");
    if (chunk.size % recordSize !== 0) {
      throw new Error("Invalid record size in SF2 chunk " + label + ".");
    }
    var result = [];
    for (var offset = 0; offset < chunk.size; offset += recordSize) {
      result.push(reader(view, chunk.data + offset));
    }
    return result;
  }

  function readBags(view, chunk, label) {
    return records(view, chunk, 4, label, function (v, p) {
      return { genIndex: v.getUint16(p, true), modIndex: v.getUint16(p + 2, true) };
    });
  }

  function readGenerators(view, chunk, label) {
    return records(view, chunk, 4, label, function (v, p) {
      var op = v.getUint16(p, true);
      var raw = v.getUint16(p + 2, true);
      var amount = (RANGE_OPERATORS.indexOf(op) >= 0 || UNSIGNED_OPERATORS.indexOf(op) >= 0) ?
        raw : v.getInt16(p + 2, true);
      return { op: op, raw: raw, amount: amount };
    });
  }

  function readModulatorCount(chunk, label) {
    if (!chunk) return 0;
    if (chunk.size % 10 !== 0 || chunk.size < 10) {
      throw new Error("Invalid " + label + " modulator chunk.");
    }
    return Math.max(0, chunk.size / 10 - 1);
  }

  function makeMap(zone) {
    var map = Object.create(null);
    for (var i = 0; i < zone.length; i++) map[zone[i].op] = zone[i];
    return map;
  }

  function amount(map, op, fallback) {
    var item = map[op];
    return item ? item.amount : fallback;
  }

  function rangeFromMap(map, op) {
    var item = map[op];
    if (!item) return { low: 0, high: 127 };
    return { low: item.raw & 255, high: (item.raw >>> 8) & 255 };
  }

  function intersectRange(a, b) {
    var result = { low: Math.max(a.low, b.low), high: Math.min(a.high, b.high) };
    return result.low <= result.high ? result : null;
  }

  function intersectZones(a, b) {
    return {
      key: intersectRange(a.key, b.key),
      velocity: intersectRange(a.velocity, b.velocity)
    };
  }

  function extractZoneRanges(globalZone, localZone) {
    var globalMap = globalZone ? makeMap(globalZone) : Object.create(null);
    var localMap = makeMap(localZone);
    return intersectZones(
      { key: rangeFromMap(globalMap, GEN.keyRange), velocity: rangeFromMap(globalMap, GEN.velocityRange) },
      { key: rangeFromMap(localMap, GEN.keyRange), velocity: rangeFromMap(localMap, GEN.velocityRange) }
    );
  }

  function splitZones(zoneList, indexOperator, label) {
    var globalZone = null;
    var localZones = [];
    for (var i = 0; i < zoneList.length; i++) {
      var hasIndex = amount(makeMap(zoneList[i]), indexOperator, null) !== null;
      if (!hasIndex) {
        if (globalZone || localZones.length !== 0) {
          throw new Error("Invalid " + label + " structure: a global zone must be first and unique.");
        }
        globalZone = zoneList[i];
      } else {
        localZones.push(zoneList[i]);
      }
    }
    return { globalZone: globalZone, localZones: localZones };
  }

  function zoneGenerators(bags, generatorList, firstBag, nextBag, label) {
    if (firstBag < 0 || nextBag <= firstBag || nextBag >= bags.length) {
      throw new Error("Invalid bag index while reading " + label + " zones.");
    }
    var result = [];
    for (var i = firstBag; i < nextBag; i++) {
      var first = bags[i].genIndex;
      var last = bags[i + 1].genIndex;
      if (last < first || last > generatorList.length) {
        throw new Error("Invalid generator index in " + label + ".");
      }
      result.push(generatorList.slice(first, last));
    }
    return result;
  }

  function copyDefaults() {
    var result = Object.create(null);
    Object.keys(DEFAULTS).forEach(function (key) {
      result[key] = DEFAULTS[key];
    });
    return result;
  }

  function combineInstrumentGenerators(globalZone, localZone) {
    var result = copyDefaults();
    var globalMap = globalZone ? makeMap(globalZone) : Object.create(null);
    var localMap = makeMap(localZone);
    Object.keys(globalMap).forEach(function (key) { result[key] = globalMap[key].amount; });
    Object.keys(localMap).forEach(function (key) { result[key] = localMap[key].amount; });
    return result;
  }

  function combinePresetGenerators(globalZone, localZone) {
    var result = Object.create(null);
    var all = [];
    if (globalZone) all = all.concat(globalZone);
    if (localZone) all = all.concat(localZone);
    for (var i = 0; i < all.length; i++) {
      var op = all[i].op;
      if (PRESET_IGNORED_OPERATORS.indexOf(op) >= 0 || RANGE_OPERATORS.indexOf(op) >= 0) continue;
      result[op] = (result[op] || 0) + all[i].amount;
    }
    return result;
  }

  function applyPresetOffsets(instrumentValues, presetValues) {
    var result = Object.create(null);
    Object.keys(instrumentValues).forEach(function (key) { result[key] = instrumentValues[key]; });
    Object.keys(presetValues).forEach(function (key) {
      var op = Number(key);
      result[key] = (result[key] || 0) + presetValues[key];
    });
    return result;
  }

  function parseSoundFont(buffer) {
    var view = new DataView(buffer);
    if (buffer.byteLength < 12 || fourCC(view, 0) !== "RIFF" || fourCC(view, 8) !== "sfbk") {
      throw new Error("The selected file is not a RIFF SoundFont 2 (sfbk) file.");
    }
    var riffEnd = view.getUint32(4, true) + 8;
    if (riffEnd < 12 || riffEnd > buffer.byteLength) {
      throw new Error("The RIFF size field is invalid or the file is truncated.");
    }

    var top = readChunks(view, 12, riffEnd);
    var infoChunks = findList(view, top, "INFO");
    var sdta = findList(view, top, "sdta");
    var pdta = findList(view, top, "pdta");
    if (!sdta || !pdta) throw new Error("The SoundFont is missing sdta or pdta.");

    var smpl = findChunk(sdta, "smpl");
    var sm24 = findChunk(sdta, "sm24");
    if (!smpl || (smpl.size & 1)) throw new Error("The SoundFont has no valid 16-bit smpl data.");
    var sampleCount = smpl.size / 2;
    if (sm24 && sm24.size < sampleCount) sm24 = null;

    var info = {};
    if (infoChunks) {
      for (var i = 0; i < infoChunks.length; i++) {
        var item = infoChunks[i];
        if (item.id === "ifil" && item.size >= 4) {
          info.Version = view.getUint16(item.data, true) + "." + view.getUint16(item.data + 2, true);
        } else if (item.id !== "LIST") {
          var value = readText(view, item);
          if (value) info[item.id] = value;
        }
      }
    }

    var phdr = records(view, findChunk(pdta, "phdr"), 38, "phdr", function (v, p) {
      return { name: readName(v, p, 20), program: v.getUint16(p + 20, true),
        bank: v.getUint16(p + 22, true), bag: v.getUint16(p + 24, true) };
    });
    var pbag = readBags(view, findChunk(pdta, "pbag"), "pbag");
    var pgen = readGenerators(view, findChunk(pdta, "pgen"), "pgen");
    var inst = records(view, findChunk(pdta, "inst"), 22, "inst", function (v, p) {
      return { name: readName(v, p, 20), bag: v.getUint16(p + 20, true) };
    });
    var ibag = readBags(view, findChunk(pdta, "ibag"), "ibag");
    var igen = readGenerators(view, findChunk(pdta, "igen"), "igen");
    var shdr = records(view, findChunk(pdta, "shdr"), 46, "shdr", function (v, p) {
      return {
        name: readName(v, p, 20),
        start: v.getUint32(p + 20, true),
        end: v.getUint32(p + 24, true),
        loopStart: v.getUint32(p + 28, true),
        loopEnd: v.getUint32(p + 32, true),
        sampleRate: v.getUint32(p + 36, true),
        originalPitch: v.getUint8(p + 40),
        pitchCorrection: v.getInt8(p + 41),
        link: v.getUint16(p + 42, true),
        type: v.getUint16(p + 44, true)
      };
    });

    if (phdr.length < 2 || inst.length < 2 || shdr.length < 2 ||
        pbag.length < 1 || ibag.length < 1) {
      throw new Error("The SoundFont preset, instrument or sample tables are incomplete.");
    }

    var instruments = [];
    for (var n = 0; n < inst.length - 1; n++) {
      var rawZones = zoneGenerators(ibag, igen, inst[n].bag, inst[n + 1].bag, "instrument");
      var split = splitZones(rawZones, GEN.sampleId, "instrument");
      instruments.push({
        name: inst[n].name || "Unnamed instrument",
        globalZone: split.globalZone,
        zones: split.localZones
      });
    }

    var presets = [];
    for (var p = 0; p < phdr.length - 1; p++) {
      var rawPresetZones = zoneGenerators(pbag, pgen, phdr[p].bag, phdr[p + 1].bag, "preset");
      var presetSplit = splitZones(rawPresetZones, GEN.instrument, "preset");
      var preset = {
        name: phdr[p].name || "Unnamed preset",
        bank: phdr[p].bank,
        program: phdr[p].program,
        zones: [],
        warnings: []
      };

      for (var z = 0; z < presetSplit.localZones.length; z++) {
        var pz = presetSplit.localZones[z];
        var pzMap = makeMap(pz);
        var instrumentIndex = amount(pzMap, GEN.instrument, -1);
        var instrument = instruments[instrumentIndex];
        if (!instrument) continue;

        var presetRanges = extractZoneRanges(presetSplit.globalZone, pz);
        if (!presetRanges.key || !presetRanges.velocity) continue;
        var presetOffsets = combinePresetGenerators(presetSplit.globalZone, pz);

        for (var iz = 0; iz < instrument.zones.length; iz++) {
          var instZone = instrument.zones[iz];
          var instMap = makeMap(instZone);
          var sampleIndex = amount(instMap, GEN.sampleId, -1);
          if (sampleIndex < 0 || sampleIndex >= shdr.length - 1) continue;

          var instrumentRanges = extractZoneRanges(instrument.globalZone, instZone);
          if (!instrumentRanges.key || !instrumentRanges.velocity) continue;
          var keyRange = intersectRange(presetRanges.key, instrumentRanges.key);
          var velocityRange = intersectRange(presetRanges.velocity, instrumentRanges.velocity);
          if (!keyRange || !velocityRange) continue;

          var instrumentValues = combineInstrumentGenerators(instrument.globalZone, instZone);
          var values = applyPresetOffsets(instrumentValues, presetOffsets);
          var unsupported = [];
          Object.keys(values).forEach(function (key) {
            var op = Number(key);
            if ((SUPPORTED_OPERATORS.indexOf(op) < 0 && values[key] !== 0) ||
                (op === GEN.velocity && values[key] >= 0)) {
              unsupported.push(op);
            }
          });

          preset.zones.push({
            instrumentName: instrument.name,
            sampleIndex: sampleIndex,
            keyRange: keyRange,
            velocityRange: velocityRange,
            values: values,
            unsupportedGenerators: unsupported
          });
        }
      }

      // Report each unsupported generator once per preset, not once per zone.
      // Large instruments commonly repeat the same generator in hundreds of zones.
      var unsupportedOperators = Object.create(null);
      for (var wi = 0; wi < preset.zones.length; wi++) {
        var unsupportedForZone = preset.zones[wi].unsupportedGenerators;
        for (var ui = 0; ui < unsupportedForZone.length; ui++) {
          unsupportedOperators[unsupportedForZone[ui]] = true;
        }
      }
      var unsupportedList = Object.keys(unsupportedOperators).map(Number).sort(function (a, b) {
        return a - b;
      });
      if (unsupportedList.length) {
        var generatorLabels = {
          5: "modLfoToPitch",
          6: "vibLfoToPitch",
          7: "modEnvToPitch",
          8: "initialFilterFc",
          9: "initialFilterQ",
          10: "modLfoToFilterFc",
          11: "modEnvToFilterFc",
          13: "modLfoToVolume",
          15: "chorusEffectsSend",
          16: "reverbEffectsSend",
          17: "pan",
          47: "fixed velocity override"
        };
        var descriptions = unsupportedList.map(function (op) {
          return op + (generatorLabels[op] ? " (" + generatorLabels[op] + ")" : "");
        });
        preset.warnings.push("Not rendered by the wavetable engine: SF2 generator(s) " +
          descriptions.join(", ") + ".");
      }
      presets.push(preset);
    }

    presets.sort(function (a, b) {
      return a.bank - b.bank || a.program - b.program || a.name.localeCompare(b.name);
    });

    var pmodCount = readModulatorCount(findChunk(pdta, "pmod"), "pmod");
    var imodCount = readModulatorCount(findChunk(pdta, "imod"), "imod");

    return {
      view: view,
      sampleDataChunk: smpl,
      sample24Chunk: sm24,
      sampleCount: sampleCount,
      samples: shdr,
      presets: presets,
      info: info,
      instrumentCount: instruments.length,
      modulatorCount: pmodCount + imodCount,
      pcmCache: Object.create(null),
      audioBufferCache: Object.create(null),
      audioBufferFrameCount: 0
    };
  }

  function midiFrequency(note) {
    return 440 * Math.pow(2, (note - 69) / 12);
  }

  function midiName(note) {
    var names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    return names[note % 12] + (Math.floor(note / 12) - 1);
  }

  function safeIdentifier(value) {
    var name = String(value || "SoundFontInstrument").trim().replace(/[^A-Za-z0-9_]/g, "_");
    if (!name) name = "SoundFontInstrument";
    if (/^[0-9]/.test(name)) name = "_" + name;
    return name;
  }

  function samplePair(soundFont, sampleIndex) {
    var sample = soundFont.samples[sampleIndex];
    if (!sample) return { error: "Invalid sample index." };
    if (sample.type & 0x8000) return { error: "ROM samples are not stored in this SF2 file." };
    if (sample.type & 0x0010) return { error: "SF3/Ogg-compressed samples are not supported by this SF2 tool." };
    if (!sample.sampleRate) return { error: "Sample rate is zero." };

    var type = sample.type & 0x000f;
    if (type === 1) return { left: sample, right: null, name: sample.name };
    if (type !== 2 && type !== 4) {
      return { error: "Unsupported sample type " + type + "; only mono and linked left/right samples are supported." };
    }

    var linked = soundFont.samples[sample.link];
    if (!linked || (linked.type & 0x000f) !== (type === 4 ? 2 : 4)) {
      return { error: "The linked stereo sample header is missing or inconsistent." };
    }
    var left = type === 4 ? sample : linked;
    var right = type === 2 ? sample : linked;
    if (left.sampleRate !== right.sampleRate) {
      return { error: "Linked stereo samples use different sample rates." };
    }
    return { left: left, right: right, name: left.name || sample.name };
  }

  function sampleOffsets(values) {
    return {
      start: values[GEN.startOffset] + values[GEN.startCoarse] * 32768,
      end: values[GEN.endOffset] + values[GEN.endCoarse] * 32768,
      loopStart: values[GEN.startLoopOffset] + values[GEN.startLoopCoarse] * 32768,
      loopEnd: values[GEN.endLoopOffset] + values[GEN.endLoopCoarse] * 32768
    };
  }

  function makeRegionLayout(soundFont, region) {
    if (region.layout) return region.layout;
    var pair = samplePair(soundFont, region.sampleIndex);
    if (pair.error) return (region.layout = { error: pair.error });

    var offsets = sampleOffsets(region.values);
    var leftStart = pair.left.start + offsets.start;
    var leftEnd = pair.left.end + offsets.end;
    var leftLoopStart = pair.left.loopStart + offsets.loopStart;
    var leftLoopEnd = pair.left.loopEnd + offsets.loopEnd;

    var rightStart = pair.right ? pair.right.start + offsets.start : 0;
    var rightEnd = pair.right ? pair.right.end + offsets.end : 0;
    var start = leftStart;
    var frames = leftEnd - leftStart;
    if (pair.right) {
      start = leftStart;
      frames = Math.min(frames, rightEnd - rightStart);
    }

    if (!Number.isInteger(start) || !Number.isInteger(frames) ||
        start < 0 || frames < 1 || leftEnd > soundFont.sampleCount ||
        (pair.right && (rightStart < 0 || rightEnd > soundFont.sampleCount))) {
      return (region.layout = { error: "Sample address generators produce an invalid sample range." });
    }

    var mode = region.values[GEN.sampleModes] & 3;
    var loopStart = leftLoopStart - leftStart;
    var loopEnd = leftLoopEnd - leftStart;
    var loopValid = loopStart >= 0 && loopEnd > loopStart && loopEnd <= frames;
    var loop = (mode === 1 || mode === 3) && loopValid;

    if (pair.right) {
      var rightLoopStart = (pair.right.loopStart + offsets.loopStart) - rightStart;
      var rightLoopEnd = (pair.right.loopEnd + offsets.loopEnd) - rightStart;
      if (rightLoopStart !== loopStart || rightLoopEnd !== loopEnd) {
        loopValid = false;
        loop = false;
      }
    }

    return (region.layout = {
      left: pair.left,
      right: pair.right,
      sampleName: pair.name,
      start: start,
      rightStart: rightStart,
      frames: frames,
      sampleRate: pair.left.sampleRate,
      loopMode: mode,
      loopStart: loopValid ? loopStart : 0,
      loopEnd: loopValid ? loopEnd : 0,
      loop: loop,
      loopValid: loopValid,
      stereoDownmix: !!pair.right,
      error: null
    });
  }

  function readSampleValue(soundFont, absoluteIndex) {
    var high = soundFont.view.getInt16(soundFont.sampleDataChunk.data + absoluteIndex * 2, true);
    if (!soundFont.sample24Chunk) return high;
    var low = soundFont.view.getUint8(soundFont.sample24Chunk.data + absoluteIndex);
    var value = high + (low >= 128 ? 1 : 0);
    return Math.max(-32768, Math.min(32767, value));
  }

  function getPcm(soundFont, region) {
    if (region.pcm) return region.pcm;
    var layout = makeRegionLayout(soundFont, region);
    if (layout.error) throw new Error(layout.error);
    var cacheKey = region.sampleIndex + ":" + layout.start + ":" + layout.frames +
      ":" + layout.loopStart + ":" + layout.loopEnd;
    if (soundFont.pcmCache[cacheKey]) {
      region.pcm = soundFont.pcmCache[cacheKey];
      return region.pcm;
    }

    var pcm = new Int16Array(layout.frames + 1);
    for (var i = 0; i < layout.frames; i++) {
      var left = readSampleValue(soundFont, layout.start + i);
      if (layout.stereoDownmix) {
        var right = readSampleValue(soundFont, layout.rightStart + i);
        pcm[i] = Math.max(-32768, Math.min(32767, Math.round((left + right) * 0.5)));
      } else {
        pcm[i] = left;
      }
    }
    pcm[layout.frames] = layout.loop && layout.loopEnd === layout.frames ?
      pcm[layout.loopStart] : 0;
    soundFont.pcmCache[cacheKey] = pcm;
    region.pcm = pcm;
    return pcm;
  }

  function getStereoPcm(soundFont, region) {
    var layout = makeRegionLayout(soundFont, region);
    if (layout.error) throw new Error(layout.error);

    var guardFrame = layout.loop && layout.loopEnd === layout.frames ? layout.loopStart : -1;
    var cacheKey = (layout.right ? "stereo:" : "mono:") + layout.start + ":" +
      layout.rightStart + ":" + layout.frames + ":" + guardFrame;
    var leftKey = cacheKey + ":left";
    var rightKey = cacheKey + ":right";
    var left = soundFont.pcmCache[leftKey];
    var right = layout.right ? soundFont.pcmCache[rightKey] : null;

    if (!left) {
      left = new Int16Array(layout.frames + 1);
      for (var i = 0; i < layout.frames; i++) {
        left[i] = readSampleValue(soundFont, layout.start + i);
      }
      left[layout.frames] = layout.loop && layout.loopEnd === layout.frames ?
        left[layout.loopStart] : 0;
      soundFont.pcmCache[leftKey] = left;
    }

    if (layout.right && !right) {
      right = new Int16Array(layout.frames + 1);
      for (var r = 0; r < layout.frames; r++) {
        right[r] = readSampleValue(soundFont, layout.rightStart + r);
      }
      right[layout.frames] = layout.loop && layout.loopEnd === layout.frames ?
        right[layout.loopStart] : 0;
      soundFont.pcmCache[rightKey] = right;
    }

    return { left: left, right: right, stereo: !!layout.right };
  }

  function timeSeconds(timecents) {
    if (timecents <= -32768) return 0;
    return Math.pow(2, timecents / 1200);
  }

  function effectiveKey(values, note) {
    var key = values[GEN.keynum];
    return key >= 0 && key <= 127 ? key : note;
  }

  function envelopeForNote(values, note) {
    var trackingKey = effectiveKey(values, note);
    var holdTc = values[GEN.holdVolEnv] - (trackingKey - 60) * values[GEN.keyToVolHold];
    var decayTc = values[GEN.decayVolEnv] - (trackingKey - 60) * values[GEN.keyToVolDecay];
    return {
      delay: timeSeconds(values[GEN.delayVolEnv]),
      attack: timeSeconds(values[GEN.attackVolEnv]),
      hold: timeSeconds(holdTc),
      decay: timeSeconds(decayTc),
      release: timeSeconds(values[GEN.releaseVolEnv]),
      sustainCb: Math.max(0, Math.min(1440, values[GEN.sustainVolEnv]))
    };
  }

  function rootKeyForRegion(soundFont, region) {
    var override = region.values[GEN.rootKey];
    if (override >= 0 && override <= 127) return override;
    var pitch = soundFont.samples[region.sampleIndex].originalPitch;
    return pitch <= 127 ? pitch : 60;
  }

  function playbackRatio(soundFont, region, note) {
    var sample = soundFont.samples[region.sampleIndex];
    var values = region.values;
    var rootKey = rootKeyForRegion(soundFont, region);
    var pitchKey = effectiveKey(values, note);
    var scaleTuning = values[GEN.scaleTuning];
    var cents = (pitchKey - rootKey) * scaleTuning +
      sample.pitchCorrection + values[GEN.coarseTune] * 100 + values[GEN.fineTune];
    return Math.pow(2, cents / 1200);
  }

  function regionsForNote(preset, note, velocity) {
    var matches = [];
    for (var i = 0; i < preset.zones.length; i++) {
      var region = preset.zones[i];
      if (note >= region.keyRange.low && note <= region.keyRange.high &&
          velocity >= region.velocityRange.low && velocity <= region.velocityRange.high) {
        matches.push(region);
      }
    }
    return matches;
  }

  var ANALYZER_GRID_STRIDE = 129;
  var ANALYZER_COMBINATION_COUNT = 128 * 127;

  function addAnalyzerRectangle(grid, keyRange, velocityRange) {
    var stride = ANALYZER_GRID_STRIDE;
    var keyStart = keyRange.low;
    var keyEnd = keyRange.high + 1;
    var velocityStart = velocityRange.low;
    var velocityEnd = velocityRange.high + 1;

    grid[keyStart * stride + velocityStart]++;
    grid[keyEnd * stride + velocityStart]--;
    grid[keyStart * stride + velocityEnd]--;
    grid[keyEnd * stride + velocityEnd]++;
  }

  function analyzerSampleInfo(soundFont, region) {
    var sample = soundFont.samples[region.sampleIndex];
    if (!sample) {
      return {
        stereo: false, stereoDeclared: false, invalidStereoLink: false,
        rom: false, compressed: false, linked: false, invalid: true
      };
    }

    var sampleType = sample.type & 0x000f;
    var stereoDeclared = sampleType === 2 || sampleType === 4;
    var linked = stereoDeclared ? soundFont.samples[sample.link] : null;
    var expectedType = sampleType === 4 ? 2 : 4;
    var stereoValid = stereoDeclared && linked &&
      (linked.type & 0x000f) === expectedType &&
      linked.sampleRate === sample.sampleRate;

    return {
      stereo: !!stereoValid,
      stereoDeclared: stereoDeclared,
      invalidStereoLink: stereoDeclared && !stereoValid,
      rom: !!(sample.type & 0x8000),
      compressed: !!(sample.type & 0x0010),
      linked: sampleType === 8,
      invalid: sampleType !== 1 && sampleType !== 2 &&
        sampleType !== 4 && sampleType !== 8
    };
  }

  function prefixAnalyzerGrid(grid) {
    var stride = ANALYZER_GRID_STRIDE;
    for (var note = 0; note < 128; note++) {
      var row = note * stride;
      var above = row - stride;
      for (var velocity = 0; velocity <= 127; velocity++) {
        var index = row + velocity;
        var value = grid[index];
        if (note > 0) value += grid[above + velocity];
        if (velocity > 0) value += grid[index - 1];
        if (note > 0 && velocity > 0) value -= grid[above + velocity - 1];
        grid[index] = value;
      }
    }
  }

  function formatVelocityRanges(flags) {
    var ranges = [];
    var start = -1;
    for (var velocity = 1; velocity <= 128; velocity++) {
      var active = velocity <= 127 && flags[velocity];
      if (active && start < 0) {
        start = velocity;
      } else if (!active && start >= 0) {
        ranges.push(start === velocity - 1 ?
          String(start) : start + "–" + (velocity - 1));
        start = -1;
      }
    }
    return ranges.length ? ranges.join(", ") : "—";
  }

  function analyzePresetVoices(preset, retainGrid) {
    if (preset.voiceAnalysis) return preset.voiceAnalysis;
    var keepGrid = retainGrid !== false;

    var layerGrid = new Int32Array(ANALYZER_GRID_STRIDE * ANALYZER_GRID_STRIDE);
    var stereoGrid = new Int32Array(ANALYZER_GRID_STRIDE * ANALYZER_GRID_STRIDE);
    var sampleSet = Object.create(null);
    var sampleHeaderSet = Object.create(null);
    var pcmSampleSet = Object.create(null);
    var instrumentSet = Object.create(null);
    var uniquePcmSamples = 0;
    var oversizedPcmSamples = 0;
    var invalidSampleLayouts = 0;
    var maxSampleFrames = 0;
    var longestSampleName = "";
    var stereoZones = 0;
    var invalidStereoLinks = 0;
    var romZones = 0;
    var compressedZones = 0;
    var linkedZones = 0;
    var invalidTypeZones = 0;

    // Each resolved preset zone is one potential musical layer. A valid
    // linked left/right pair remains one layer but needs two PCM channel reads.
    for (var i = 0; i < preset.zones.length; i++) {
      var region = preset.zones[i];
      var sample = font.samples[region.sampleIndex];
      if (!sample) continue;

      var sampleInfo = analyzerSampleInfo(font, region);

      // Use the same PCM identity as the V2 exporter. This counts unique
      // effective sample data ranges after SF2 address generators and stereo
      // pairing, rather than counting a reused zone multiple times.
      var layout = makeRegionLayout(font, region);
      if (layout.error) {
        invalidSampleLayouts++;
      } else {
        var pcmGuardFrame = layout.loop && layout.loopEnd === layout.frames ?
          layout.loopStart : -1;
        var pcmKey = (layout.right ? "stereo:" : "mono:") + layout.start + ":" +
          layout.rightStart + ":" + layout.frames + ":" + pcmGuardFrame;
        if (!pcmSampleSet[pcmKey]) {
          pcmSampleSet[pcmKey] = true;
          uniquePcmSamples++;
          if (layout.frames > MAX_SAMPLE_FRAMES_V2) oversizedPcmSamples++;
          if (layout.frames > maxSampleFrames) {
            maxSampleFrames = layout.frames;
            longestSampleName = layout.sampleName || sample.name || region.instrumentName;
          }
        }
      }

      addAnalyzerRectangle(layerGrid, region.keyRange, region.velocityRange);
      if (sampleInfo.stereo) {
        addAnalyzerRectangle(stereoGrid, region.keyRange, region.velocityRange);
        stereoZones++;
      }
      if (sampleInfo.invalidStereoLink) invalidStereoLinks++;
      if (sampleInfo.rom) romZones++;
      if (sampleInfo.compressed) compressedZones++;
      if (sampleInfo.linked) linkedZones++;
      if (sampleInfo.invalid) invalidTypeZones++;
      instrumentSet[region.instrumentName] = true;

      sampleHeaderSet[region.sampleIndex] = true;
      if (sampleInfo.stereo) sampleHeaderSet[sample.link] = true;

      var sampleType = sample.type & 0x000f;
      var sampleKey = sampleInfo.stereo ?
        "stereo:" + Math.min(region.sampleIndex, sample.link) + ":" +
          Math.max(region.sampleIndex, sample.link) :
        "sample:" + region.sampleIndex;
      if (sampleType === 8) sampleKey = "linked:" + region.sampleIndex;
      sampleSet[sampleKey] = true;
    }

    // Two-dimensional prefix sums turn all key/velocity rectangles into a
    // complete 128 x 127 activation map in O(zones + 16,256) time.
    prefixAnalyzerGrid(layerGrid);
    prefixAnalyzerGrid(stereoGrid);

    var histogram = [0, 0, 0, 0, 0, 0];
    var perNote = [];
    var maxLayers = 0;
    var maxLayerCells = 0;
    var maxChannels = 0;
    var maxChannelCells = 0;
    var coveredCells = 0;
    var totalLayers = 0;
    var totalPcmChannels = 0;
    var moreThanV2LimitCells = 0;
    var moreThanEightLayerCells = 0;
    var notesOverV2Limit = 0;
    var notesOverEightLayers = 0;

    for (var note = 0; note < 128; note++) {
      var noteMaxLayers = 0;
      var noteMaxChannels = 0;
      var peakLayerVelocities = new Uint8Array(128);
      var peakChannelVelocities = new Uint8Array(128);

      for (var velocity = 1; velocity <= 127; velocity++) {
        var index = note * ANALYZER_GRID_STRIDE + velocity;
        var layers = layerGrid[index];
        var stereoLayers = stereoGrid[index];
        var channels = layers + stereoLayers;

        histogram[layers >= 5 ? 5 : layers]++;
        totalLayers += layers;
        totalPcmChannels += channels;
        if (layers > 0) coveredCells++;
        if (layers > V2_MAX_LAYERS_PER_NOTE) moreThanV2LimitCells++;
        if (layers > MAX_PREVIEW_LAYERS_PER_NOTE) moreThanEightLayerCells++;

        if (layers > noteMaxLayers) {
          noteMaxLayers = layers;
          peakLayerVelocities.fill(0);
          peakLayerVelocities[velocity] = 1;
        } else if (layers === noteMaxLayers && layers > 0) {
          peakLayerVelocities[velocity] = 1;
        }

        if (channels > noteMaxChannels) {
          noteMaxChannels = channels;
          peakChannelVelocities.fill(0);
          peakChannelVelocities[velocity] = 1;
        } else if (channels === noteMaxChannels && channels > 0) {
          peakChannelVelocities[velocity] = 1;
        }

        if (layers > maxLayers) {
          maxLayers = layers;
          maxLayerCells = 1;
        } else if (layers === maxLayers && layers > 0) {
          maxLayerCells++;
        }

        if (channels > maxChannels) {
          maxChannels = channels;
          maxChannelCells = 1;
        } else if (channels === maxChannels && channels > 0) {
          maxChannelCells++;
        }
      }

      perNote.push({
        note: note,
        maxLayers: noteMaxLayers,
        maxChannels: noteMaxChannels,
        peakLayerRange: formatVelocityRanges(peakLayerVelocities),
        peakChannelRange: formatVelocityRanges(peakChannelVelocities)
      });
      if (noteMaxLayers > V2_MAX_LAYERS_PER_NOTE) notesOverV2Limit++;
      if (noteMaxLayers > MAX_PREVIEW_LAYERS_PER_NOTE) notesOverEightLayers++;
    }

    var peakLayerNotes = [];
    var peakChannelNotes = [];
    for (var n = 0; n < perNote.length; n++) {
      var entry = perNote[n];
      if (maxLayers > 0 && entry.maxLayers === maxLayers) peakLayerNotes.push(entry);
      if (maxChannels > 0 && entry.maxChannels === maxChannels) peakChannelNotes.push(entry);
    }

    var result = {
      zoneCount: preset.zones.length,
      instrumentCount: Object.keys(instrumentSet).length,
      uniqueSampleSets: Object.keys(sampleSet).length,
      sampleHeaders: Object.keys(sampleHeaderSet).length,
      uniquePcmSamples: uniquePcmSamples,
      oversizedPcmSamples: oversizedPcmSamples,
      invalidSampleLayouts: invalidSampleLayouts,
      maxSampleFrames: maxSampleFrames,
      longestSampleName: longestSampleName,
      stereoZones: stereoZones,
      invalidStereoLinks: invalidStereoLinks,
      romZones: romZones,
      compressedZones: compressedZones,
      linkedZones: linkedZones,
      invalidTypeZones: invalidTypeZones,
      maxLayers: maxLayers,
      maxLayerCells: maxLayerCells,
      maxChannels: maxChannels,
      maxChannelCells: maxChannelCells,
      coveredCells: coveredCells,
      totalLayers: totalLayers,
      totalPcmChannels: totalPcmChannels,
      histogram: histogram,
      moreThanV2LimitCells: moreThanV2LimitCells,
      moreThanEightLayerCells: moreThanEightLayerCells,
      notesOverV2Limit: notesOverV2Limit,
      notesOverEightLayers: notesOverEightLayers,
      peakLayerNotes: peakLayerNotes,
      peakChannelNotes: peakChannelNotes,
      perNote: perNote
    };

    if (keepGrid) {
      result.layerGrid = layerGrid;
      result.stereoGrid = stereoGrid;
      preset.voiceAnalysis = result;
    }
    return result;
  }

  function appendAnalyzerMetric(parent, label, value, detail) {
    var metric = document.createElement("div");
    metric.className = "sf2-analyzer-metric";

    var valueElement = document.createElement("strong");
    valueElement.textContent = value;
    var labelElement = document.createElement("span");
    labelElement.textContent = label;
    metric.appendChild(valueElement);
    metric.appendChild(labelElement);

    if (detail) {
      var detailElement = document.createElement("small");
      detailElement.textContent = detail;
      metric.appendChild(detailElement);
    }
    parent.appendChild(metric);
  }

  function appendAnalyzerTableCell(row, value, header) {
    var cell = document.createElement(header ? "th" : "td");
    cell.textContent = value;
    row.appendChild(cell);
  }

  function renderPresetVoiceAnalyzer(preset) {
    if (!analyzerPanel) return;
    analyzerPanel.textContent = "";
    if (!preset) return;

    var analysis = analyzePresetVoices(preset);
    var selectedVelocity = currentVelocity();
    var selectedNoteValue = parseInt(noteInput.value, 10);
    var selectedNote = Math.max(0, Math.min(127,
      isNaN(selectedNoteValue) ? 60 : selectedNoteValue));
    var selectedCoveredNotes = 0;
    var selectedOverlappingNotes = 0;
    var selectedMaxLayers = 0;
    var selectedMaxLayerNote = -1;
    var selectedMaxChannels = 0;
    var selectedMaxChannelNote = -1;

    for (var i = 0; i < analysis.perNote.length; i++) {
      var item = analysis.perNote[i];
      var gridIndex = item.note * ANALYZER_GRID_STRIDE + selectedVelocity;
      item.selectedLayers = analysis.layerGrid[gridIndex];
      item.selectedStereoLayers = analysis.stereoGrid[gridIndex];
      item.selectedChannels = item.selectedLayers + item.selectedStereoLayers;

      if (item.selectedLayers > 0) selectedCoveredNotes++;
      if (item.selectedLayers > 1) selectedOverlappingNotes++;
      if (item.selectedLayers > selectedMaxLayers) {
        selectedMaxLayers = item.selectedLayers;
        selectedMaxLayerNote = item.note;
      }
      if (item.selectedChannels > selectedMaxChannels) {
        selectedMaxChannels = item.selectedChannels;
        selectedMaxChannelNote = item.note;
      }
    }

    var heading = document.createElement("h3");
    heading.textContent = "Preset / Voice analyzer";
    analyzerPanel.appendChild(heading);

    var intro = document.createElement("p");
    intro.className = "help";
    intro.textContent =
      "All 128 MIDI notes × 127 playable velocity values are checked. " +
      "A resolved zone counts as one musical layer; a valid linked stereo pair counts as two PCM channels.";
    analyzerPanel.appendChild(intro);

    var metrics = document.createElement("div");
    metrics.className = "sf2-analyzer-grid";
    appendAnalyzerMetric(metrics, "Maximum simultaneous layers",
      String(analysis.maxLayers),
      analysis.peakLayerNotes.length ?
        analysis.peakLayerNotes.slice(0, 6).map(function (entry) {
          return midiName(entry.note) + " (velocity " + entry.peakLayerRange + ")";
        }).join(" · ") +
        (analysis.peakLayerNotes.length > 6 ? " · +" + (analysis.peakLayerNotes.length - 6) + " notes" : "") :
        "No playable key/velocity combinations.");
    appendAnalyzerMetric(metrics, "Maximum mono PCM channels",
      String(analysis.maxChannels),
      analysis.peakChannelNotes.length ?
        analysis.peakChannelNotes.slice(0, 6).map(function (entry) {
          return midiName(entry.note) + " (velocity " + entry.peakChannelRange + ")";
        }).join(" · ") +
        (analysis.peakChannelNotes.length > 6 ? " · +" + (analysis.peakChannelNotes.length - 6) + " notes" : "") :
        "No playable key/velocity combinations.");
    appendAnalyzerMetric(metrics, "Key/velocity combinations covered",
      analysis.coveredCells.toLocaleString() + " / " + ANALYZER_COMBINATION_COUNT.toLocaleString(),
      (analysis.coveredCells * 100 / ANALYZER_COMBINATION_COUNT).toFixed(1) + "% of the complete MIDI grid.");
    appendAnalyzerMetric(metrics, "Combinations above current V2 layer limit",
      analysis.moreThanV2LimitCells.toLocaleString(),
      "More than " + V2_MAX_LAYERS_PER_NOTE + " layers; " +
        analysis.notesOverV2Limit + " notes affected.");
    appendAnalyzerMetric(metrics, "Stereo sample zones",
      String(analysis.stereoZones),
      "One valid stereo zone is one musical layer but two mono PCM streams.");
    appendAnalyzerMetric(metrics, "Resolved zones / instruments / sample headers / sample sets",
      analysis.zoneCount + " / " + analysis.instrumentCount + " / " +
        analysis.sampleHeaders + " / " + analysis.uniqueSampleSets,
      "A linked stereo pair counts as two sample headers and one sample set.");
    appendAnalyzerMetric(metrics, "Unique PCM samples / over V2 frame limit",
      analysis.uniquePcmSamples + " / " + analysis.oversizedPcmSamples,
      "V2 limit: " + MAX_SAMPLE_FRAMES_V2.toLocaleString() + " frames. Longest: " +
        (analysis.maxSampleFrames ? analysis.maxSampleFrames.toLocaleString() + " frames (" +
          (analysis.longestSampleName || "unnamed sample") + ")." : "no valid sample layouts.") +
        (analysis.invalidSampleLayouts ? " Invalid sample layouts: " + analysis.invalidSampleLayouts + "." : ""));
    analyzerPanel.appendChild(metrics);

    var selectedNoteEntry = analysis.perNote[selectedNote];
    var velocityHeading = document.createElement("h4");
    velocityHeading.textContent = "At selected velocity " + selectedVelocity;
    analyzerPanel.appendChild(velocityHeading);

    var selectedSummary = document.createElement("p");
    selectedSummary.className = "sf2-analyzer-summary";
    selectedSummary.textContent =
      selectedCoveredNotes + " notes covered · " +
      selectedOverlappingNotes + " notes with multiple layers · " +
      "maximum " + selectedMaxLayers + " layers at " +
      (selectedMaxLayerNote >= 0 ? midiName(selectedMaxLayerNote) : "—") +
      " · maximum " + selectedMaxChannels + " PCM channels at " +
      (selectedMaxChannelNote >= 0 ? midiName(selectedMaxChannelNote) : "—") + ".";
    analyzerPanel.appendChild(selectedSummary);

    var selectedNoteSummary = document.createElement("p");
    selectedNoteSummary.className = "sf2-analyzer-summary";
    selectedNoteSummary.textContent =
      "Preview note " + midiName(selectedNote) + " @ velocity " +
      selectedVelocity + ": " + selectedNoteEntry.selectedLayers + " layers, " +
      selectedNoteEntry.selectedChannels + " mono PCM channels (" +
      selectedNoteEntry.selectedStereoLayers + " stereo layers).";
    analyzerPanel.appendChild(selectedNoteSummary);

    var distributionHeading = document.createElement("h4");
    distributionHeading.textContent = "Layer distribution across the complete key/velocity grid";
    analyzerPanel.appendChild(distributionHeading);

    var distribution = document.createElement("div");
    distribution.className = "sf2-analyzer-distribution";
    var bucketLabels = ["0 layers", "1 layer", "2 layers", "3 layers", "4 layers", "5+ layers"];
    for (var bucket = 0; bucket < analysis.histogram.length; bucket++) {
      var count = analysis.histogram[bucket];
      var row = document.createElement("div");
      row.className = "sf2-analyzer-bar-row";

      var label = document.createElement("span");
      label.textContent = bucketLabels[bucket];
      var track = document.createElement("div");
      track.className = "sf2-analyzer-bar-track";
      var bar = document.createElement("div");
      bar.className = "sf2-analyzer-bar";
      bar.style.width = (count * 100 / ANALYZER_COMBINATION_COUNT).toFixed(2) + "%";
      track.appendChild(bar);
      var number = document.createElement("span");
      number.textContent = count.toLocaleString() + " (" +
        (count * 100 / ANALYZER_COMBINATION_COUNT).toFixed(1) + "%)";
      row.appendChild(label);
      row.appendChild(track);
      row.appendChild(number);
      distribution.appendChild(row);
    }
    analyzerPanel.appendChild(distribution);

    var limits = document.createElement("p");
    limits.className = "help";
    limits.textContent =
      "Combinations above 8 layers (browser preview cap): " +
      analysis.moreThanEightLayerCells.toLocaleString() +
      "; above 4 layers (current V2 cap): " +
      analysis.moreThanV2LimitCells.toLocaleString() +
      ". Maximum sample channels include two reads for every valid stereo zone.";
    analyzerPanel.appendChild(limits);

    if (analysis.romZones || analysis.compressedZones || analysis.linkedZones ||
        analysis.invalidTypeZones || analysis.invalidStereoLinks) {
      var warning = document.createElement("p");
      warning.className = "sf2-analyzer-warning";
      var warningParts = [];
      if (analysis.romZones) warningParts.push(analysis.romZones + " ROM zone(s)");
      if (analysis.compressedZones) warningParts.push(analysis.compressedZones + " Ogg/SF3 zone(s)");
      if (analysis.linkedZones) warningParts.push(analysis.linkedZones + " undefined linked-sample zone(s)");
      if (analysis.invalidTypeZones) warningParts.push(analysis.invalidTypeZones + " zone(s) with unknown sample type");
      if (analysis.invalidStereoLinks) warningParts.push(analysis.invalidStereoLinks + " invalid stereo link(s)");
      warning.textContent = "Sample-format warnings: " + warningParts.join(", ") +
        ". These zones are counted by their metadata, but may not be playable by this tool.";
      analyzerPanel.appendChild(warning);
    }

    var details = document.createElement("details");
    details.className = "sf2-analyzer-details";
    var detailsSummary = document.createElement("summary");
    detailsSummary.textContent = "Show note-by-note peak layers and velocity ranges";
    details.appendChild(detailsSummary);

    var tableWrap = document.createElement("div");
    tableWrap.className = "sf2-analyzer-table-wrap";
    var table = document.createElement("table");
    var thead = document.createElement("thead");
    var headerRow = document.createElement("tr");
    ["Note", "Max layers", "Velocity range (layers)", "Max PCM channels",
      "Velocity range (PCM)", "Layers @ selected velocity",
      "PCM channels @ selected velocity"].forEach(function (value) {
      appendAnalyzerTableCell(headerRow, value, true);
    });
    thead.appendChild(headerRow);
    table.appendChild(thead);

    var tbody = document.createElement("tbody");
    for (var noteIndex = 0; noteIndex < analysis.perNote.length; noteIndex++) {
      var noteEntry = analysis.perNote[noteIndex];
      if (noteEntry.maxLayers === 0) continue;
      var noteRow = document.createElement("tr");
      appendAnalyzerTableCell(noteRow, midiName(noteEntry.note));
      appendAnalyzerTableCell(noteRow, String(noteEntry.maxLayers));
      appendAnalyzerTableCell(noteRow, noteEntry.peakLayerRange);
      appendAnalyzerTableCell(noteRow, String(noteEntry.maxChannels));
      appendAnalyzerTableCell(noteRow, noteEntry.peakChannelRange);
      appendAnalyzerTableCell(noteRow, String(noteEntry.selectedLayers));
      appendAnalyzerTableCell(noteRow, String(noteEntry.selectedChannels));
      tbody.appendChild(noteRow);
    }
    table.appendChild(tbody);
    tableWrap.appendChild(table);
    details.appendChild(tableWrap);
    analyzerPanel.appendChild(details);
  }

  function safeLogField(value) {
    return String(value === undefined || value === null ? "" : value)
      .replace(/[\t\r\n|]/g, " ")
      .replace(/\s+/g, " ")
      .trim();
  }

  function analyzerPeakNoteList(entries, rangeKey) {
    if (!entries.length) return "—";
    var limit = 12;
    var result = entries.slice(0, limit).map(function (entry) {
      return midiName(entry.note) + "@" + entry[rangeKey];
    });
    if (entries.length > limit) result.push("+" + (entries.length - limit) + " notes");
    return result.join(", ");
  }

  async function scanAllSoundFontPresets() {
    if (!font || !allPresetScanButton) return;

    var soundFont = font;
    var scanGeneration = ++allPresetScanGeneration;
    var presets = soundFont.presets;
    var results = [];
    var maxLayersOverall = 0;
    var maxChannelsOverall = 0;
    var presetsAboveV2Limit = 0;
    var presetsAboveEightLayers = 0;
    var presetsWithStereo = 0;
    var maxLayerHistogram = [0, 0, 0, 0, 0, 0];
    var totalResolvedZones = 0;
    var presetsWithOversizedSamples = 0;
    var totalUniquePcmSamples = 0;
    var totalOversizedPcmSamples = 0;
    var maxSampleFramesOverall = 0;
    var longestSamplePreset = "";
    var longestSampleNameOverall = "";

    stopPreview();
    allPresetScanButton.disabled = true;
    allPresetScanCopyButton.disabled = true;
    allPresetScanDownloadButton.disabled = true;
    allPresetScanPanel.classList.add("hidden");
    allPresetScanLog.value = "";
    allPresetScanStatus.textContent = "Starting full bank/preset scan…";

    try {
      for (var i = 0; i < presets.length; i++) {
        if (scanGeneration !== allPresetScanGeneration || soundFont !== font) return;

        var preset = presets[i];
        // Do not retain per-preset 128x129 grids during this bulk scan.
        var analysis = analyzePresetVoices(preset, false);
        var instrumentNames = Object.create(null);
        for (var z = 0; z < preset.zones.length; z++) {
          instrumentNames[preset.zones[z].instrumentName] = true;
        }
        var names = Object.keys(instrumentNames).sort(function (a, b) {
          return a.localeCompare(b);
        });

        var maxLayers = analysis.maxLayers;
        var histogramIndex = maxLayers >= 5 ? 5 : maxLayers;
        maxLayerHistogram[histogramIndex]++;
        if (maxLayers > maxLayersOverall) maxLayersOverall = maxLayers;
        if (analysis.maxChannels > maxChannelsOverall) maxChannelsOverall = analysis.maxChannels;
        if (maxLayers > V2_MAX_LAYERS_PER_NOTE) presetsAboveV2Limit++;
        if (maxLayers > MAX_PREVIEW_LAYERS_PER_NOTE) presetsAboveEightLayers++;
        if (analysis.stereoZones) presetsWithStereo++;
        totalResolvedZones += analysis.zoneCount;
        if (analysis.oversizedPcmSamples) presetsWithOversizedSamples++;
        totalUniquePcmSamples += analysis.uniquePcmSamples;
        totalOversizedPcmSamples += analysis.oversizedPcmSamples;
        if (analysis.maxSampleFrames > maxSampleFramesOverall) {
          maxSampleFramesOverall = analysis.maxSampleFrames;
          longestSamplePreset = preset.name + " (Bank " + preset.bank +
            ", Program " + preset.program + ")";
          longestSampleNameOverall = analysis.longestSampleName;
        }

        results.push({
          bank: preset.bank,
          program: preset.program,
          name: preset.name,
          zones: analysis.zoneCount,
          instrumentCount: analysis.instrumentCount,
          instruments: names.join(", "),
          sampleHeaders: analysis.sampleHeaders,
          sampleSets: analysis.uniqueSampleSets,
          uniquePcmSamples: analysis.uniquePcmSamples,
          oversizedPcmSamples: analysis.oversizedPcmSamples,
          maxSampleFrames: analysis.maxSampleFrames,
          longestSample: analysis.longestSampleName,
          invalidSampleLayouts: analysis.invalidSampleLayouts,
          maxLayers: maxLayers,
          peakLayers: analyzerPeakNoteList(analysis.peakLayerNotes, "peakLayerRange"),
          maxChannels: analysis.maxChannels,
          peakChannels: analyzerPeakNoteList(analysis.peakChannelNotes, "peakChannelRange"),
          coveredCells: analysis.coveredCells,
          overV2Cells: analysis.moreThanV2LimitCells,
          stereoZones: analysis.stereoZones,
          romZones: analysis.romZones,
          compressedZones: analysis.compressedZones,
          invalidStereoLinks: analysis.invalidStereoLinks
        });

        if ((i & 3) === 3 || i === presets.length - 1) {
          allPresetScanStatus.textContent =
            "Analyzing " + (i + 1) + " / " + presets.length +
            " presets — " + safeLogField(preset.name) +
            " (Bank " + preset.bank + ", Program " + preset.program + ")";
          await new Promise(function (resolve) { setTimeout(resolve, 0); });
        }
      }

      if (scanGeneration !== allPresetScanGeneration || soundFont !== font) return;

      results.sort(function (a, b) {
        return b.maxLayers - a.maxLayers ||
          b.maxChannels - a.maxChannels ||
          a.bank - b.bank ||
          a.program - b.program ||
          a.name.localeCompare(b.name);
      });

      var lines = [];
      lines.push("ESP-beatsy SF2 all-preset voice analysis");
      lines.push("File: " + safeLogField(currentFileName));
      lines.push("SoundFont: " + safeLogField(soundFont.info.INAM || ""));
      lines.push("SF2 version: " + safeLogField(soundFont.info.Version || ""));
      lines.push("Presets analyzed: " + results.length + " / " + presets.length);
      lines.push("SoundFont instruments: " + soundFont.instrumentCount);
      lines.push("Sample headers: " + Math.max(0, soundFont.samples.length - 1));
      lines.push("Resolved preset zones (sum over presets): " + totalResolvedZones);
      lines.push("Unique PCM samples (sum over presets, V2 exporter deduplication): " + totalUniquePcmSamples);
      lines.push("Unique PCM samples above V2 frame limit (" + MAX_SAMPLE_FRAMES_V2 + ", sum over presets): " +
        totalOversizedPcmSamples);
      lines.push("Presets with samples above V2 frame limit: " + presetsWithOversizedSamples);
      lines.push("Maximum resolved sample length: " +
        (maxSampleFramesOverall ? maxSampleFramesOverall.toLocaleString() + " frames (" +
          (longestSampleNameOverall || "unnamed sample") + "; " + longestSamplePreset + ")" : "—"));
      lines.push("Maximum simultaneous layers in any preset: " + maxLayersOverall);
      lines.push("Maximum mono PCM channels in any preset: " + maxChannelsOverall);
      lines.push("Presets by maximum layers: " +
        maxLayerHistogram.map(function (count, index) {
          return (index === 5 ? "5+" : String(index)) + "=" + count;
        }).join(", "));
      lines.push("Presets above V2 limit (" + V2_MAX_LAYERS_PER_NOTE + " layers): " + presetsAboveV2Limit);
      lines.push("Presets above browser preview limit (" + MAX_PREVIEW_LAYERS_PER_NOTE + " layers): " +
        presetsAboveEightLayers);
      lines.push("Presets containing valid stereo zones: " + presetsWithStereo);
      lines.push("");
      lines.push([
        "Bank", "Program", "Preset", "Zones", "Instrument count", "Instrument names",
        "Sample headers", "Sample sets", "Unique PCM samples", "Samples > V2 frame limit",
        "Max sample frames", "Longest sample", "Invalid sample layouts",
        "Max layers", "Notes/velocity at max layers",
        "Max PCM channels", "Notes/velocity at max PCM", "Covered key/velocity cells",
        "Cells above V2 limit", "Stereo zones", "ROM zones", "Ogg/SF3 zones",
        "Invalid stereo links"
      ].join("\t"));

      for (var r = 0; r < results.length; r++) {
        var item = results[r];
        lines.push([
          item.bank, item.program, safeLogField(item.name), item.zones,
          item.instrumentCount, safeLogField(item.instruments), item.sampleHeaders,
          item.sampleSets, item.uniquePcmSamples, item.oversizedPcmSamples,
          item.maxSampleFrames, safeLogField(item.longestSample), item.invalidSampleLayouts,
          item.maxLayers, safeLogField(item.peakLayers), item.maxChannels,
          safeLogField(item.peakChannels), item.coveredCells, item.overV2Cells,
          item.stereoZones, item.romZones, item.compressedZones, item.invalidStereoLinks
        ].join("\t"));
      }

      var report = lines.join("\n");
      allPresetScanLog.value = report;
      allPresetScanPanel.classList.remove("hidden");
      allPresetScanCopyButton.disabled = false;
      allPresetScanDownloadButton.disabled = false;
      allPresetScanStatus.textContent =
        "Completed: " + results.length + " presets. Maximum " +
        maxLayersOverall + " simultaneous layers and " + maxChannelsOverall +
        " mono PCM channels; " + presetsWithOversizedSamples +
        " preset(s) contain samples over the V2 frame limit. The report is sorted by maximum layer count.";
      console.log("ESP-beatsy SF2 all-preset voice analysis\n" + report);
    } catch (error) {
      if (scanGeneration === allPresetScanGeneration && soundFont === font) {
        allPresetScanStatus.textContent = "Full preset scan failed: " + error.message;
      }
    } finally {
      if (scanGeneration === allPresetScanGeneration && soundFont === font) {
        allPresetScanButton.disabled = false;
      }
    }
  }

  function resetAllPresetScan() {
    allPresetScanGeneration++;
    if (allPresetScanButton) allPresetScanButton.disabled = true;
    if (allPresetScanStatus) allPresetScanStatus.textContent = "";
    if (allPresetScanLog) allPresetScanLog.value = "";
    if (allPresetScanPanel) allPresetScanPanel.classList.add("hidden");
    if (allPresetScanCopyButton) allPresetScanCopyButton.disabled = true;
    if (allPresetScanDownloadButton) allPresetScanDownloadButton.disabled = true;
  }

  function updatePresetCompatibilityStatus(preset) {
    presetCompatibilityMessage = "";
    if (!font || !preset) {
      setPreviewStatus("");
      return;
    }

    var wavetableMode = isWavetableMode();
    var layerLimit = wavetableMode ? V2_MAX_LAYERS_PER_NOTE : MAX_PREVIEW_LAYERS_PER_NOTE;
    var frameLimit = wavetableMode ? MAX_SAMPLE_FRAMES_V2 : MAX_PREVIEW_SAMPLE_FRAMES;
    var analysis = analyzePresetVoices(preset);
    var problems = [];

    if (!preset.zones.length) {
      problems.push("the preset contains no resolved playable sample zones");
    }

    if (analysis.maxLayers > layerLimit) {
      var overflow = null;
      for (var note = 0; note < 128 && !overflow; note++) {
        for (var velocity = 1; velocity <= 127; velocity++) {
          var layers = analysis.layerGrid[note * ANALYZER_GRID_STRIDE + velocity];
          if (layers > layerLimit) {
            overflow = { note: note, velocity: velocity, layers: layers };
            break;
          }
        }
      }
      if (overflow) {
        problems.push(midiName(overflow.note) + " at velocity " + overflow.velocity +
          " has " + overflow.layers + " simultaneous layers (limit " + layerLimit + ")");
      }
    }

    var oversizedZones = [];
    var invalidZones = [];
    for (var zoneIndex = 0; zoneIndex < preset.zones.length; zoneIndex++) {
      var region = preset.zones[zoneIndex];
      var layout = makeRegionLayout(font, region);
      if (layout.error) {
        invalidZones.push({
          instrument: region.instrumentName,
          reason: layout.error
        });
      } else if (layout.frames > frameLimit) {
        oversizedZones.push({
          instrument: region.instrumentName,
          frames: layout.frames
        });
      }
    }

    if (oversizedZones.length) {
      var firstOversized = oversizedZones[0];
      problems.push(oversizedZones.length + " zone(s) exceed the " +
        frameLimit.toLocaleString() + "-frame sample limit; first: " +
        firstOversized.instrument + " (" +
        firstOversized.frames.toLocaleString() + " frames)");
    }
    if (invalidZones.length) {
      var firstInvalid = invalidZones[0];
      problems.push(invalidZones.length + " zone(s) have unsupported or invalid sample data; first: " +
        firstInvalid.instrument + " (" + firstInvalid.reason + ")");
    }

    if (problems.length) {
      var modeName = wavetableMode ? "V2-compatible Preview" : "extended browser preview";
      presetCompatibilityMessage = "This preset is not fully playable in the selected " +
        modeName + ": " + problems.join("; ") +
        ". The affected key/velocity regions cannot be auditioned.";
      setPreviewStatus(presetCompatibilityMessage);
    } else {
      setPreviewStatus("");
    }
  }

  function updatePresetInfo() {
    presetInfo.textContent = "";
    if (analyzerPanel) analyzerPanel.textContent = "";
    if (!font) return;
    var preset = font.presets[parseInt(presetSelect.value, 10)];
    if (!preset) return;

    var velocity = Math.max(1, Math.min(127, parseInt(velocityInput.value, 10) || 100));
    var coveredNotes = 0;
    var overlappingNotes = 0;

    for (var note = 0; note < 128; note++) {
      var matches = regionsForNote(preset, note, velocity);
      if (matches.length) coveredNotes++;
      if (matches.length > 1) overlappingNotes++;
    }

    var lines = [
      "Preset: " + preset.name,
      "Bank / program: " + preset.bank + " / " + preset.program,
      "Instrument zones: " + preset.zones.length,
      "Notes covered at velocity " + velocity + ": " + coveredNotes + " / 128",
      "Notes with overlapping layers at this velocity: " + overlappingNotes
    ];
    if (font.modulatorCount) {
      lines.push("Modulators present: " + font.modulatorCount + " (not rendered by V2).");
    }
    if (font.sample24Chunk) lines.push("24-bit sample data: sm24 found; rounded to 16-bit for this engine.");
    for (var i = 0; i < preset.warnings.length; i++) lines.push("Warning: " + preset.warnings[i]);
    if (overlappingNotes) {
      lines.push("These notes use multiple layers. V2 export preserves up to four matching layers per note and rejects presets that exceed the engine limit.");
    }
    presetInfo.textContent = lines.join("\n");
    renderPresetVoiceAnalyzer(preset);
    updatePresetCompatibilityStatus(preset);
  }

  function renderMetadata(info) {
    metadata.innerHTML = "";
    var labels = {
      INAM: "SoundFont", IENG: "Engineer", IPRD: "Product",
      ICOP: "Copyright", ICRD: "Date", ICMT: "Comment", ISFT: "Created with",
      Version: "SF2 version", isng: "Target engine"
    };
    Object.keys(info).forEach(function (key) {
      var row = document.createElement("div");
      var strong = document.createElement("strong");
      var span = document.createElement("span");
      strong.textContent = (labels[key] || key) + ": ";
      span.textContent = info[key];
      row.appendChild(strong);
      row.appendChild(span);
      metadata.appendChild(row);
    });
    if (!Object.keys(info).length) metadata.textContent = "No optional INFO metadata found.";
  }

  function populatePresets() {
    presetSelect.innerHTML = "";
    for (var i = 0; i < font.presets.length; i++) {
      var preset = font.presets[i];
      var option = document.createElement("option");
      option.value = String(i);
      option.textContent = "Bank " + preset.bank + " · Program " +
        preset.program + " · " + preset.name + " (" + preset.zones.length + " zones)";
      presetSelect.appendChild(option);
    }
    presetSelect.disabled = font.presets.length === 0;
  }

  // Lower octave: white keys on the bottom row (Z/X/C/V/B/N/M),
  // black keys on the home row (S/D/G/H/J).
  // Upper octave: white keys on the top row (Q/W/E/R/T/Y/U),
  // black keys on the number row (2/3/5/6/7).
  // KeyboardEvent.code keeps these physical positions stable across layouts.
  var KEYBOARD_PC_CODES = [
    "KeyZ", "KeyS", "KeyX", "KeyD", "KeyC", "KeyV",
    "KeyG", "KeyB", "KeyH", "KeyN", "KeyJ", "KeyM",
    "KeyQ", "Digit2", "KeyW", "Digit3", "KeyE", "KeyR",
    "Digit5", "KeyT", "Digit6", "KeyY", "Digit7", "KeyU"
  ];
  var KEYBOARD_PC_LABELS = [
    "Y", "S", "X", "D", "C", "V", "G", "B", "H", "N", "J", "M",
    "Q", "2", "W", "3", "E", "R", "5", "T", "6", "Z", "7", "U"
  ];
  var BLACK_KEY_OFFSETS = [1, 3, 6, 8, 10];
  var MAX_PREVIEW_VOICES = 16;
  var MAX_PREVIEW_LAYERS_PER_NOTE = 16;
  var MAX_PREVIEW_SAMPLE_FRAMES = 4000000;
  var MAX_PREVIEW_CACHE_FRAMES = 10000000;
  // Keep this aligned with AudioSynthWavetableV2::MAX_SAMPLE_LAYERS_PER_NOTE.
  var V2_MAX_LAYERS_PER_NOTE = 4;
  var V2_MAX_NOTES = 32;
  var MAX_EXPORT_ZONES = 65535;
  var WAVETABLE_DEFAULT_AMPLITUDE = 90 / 127;

  function keyboardLabelsForLayout(layout) {
    var labels = KEYBOARD_PC_LABELS.slice();
    if (layout === "qwerty") {
      labels[0] = "Z";
      labels[21] = "Y";
    }
    return labels;
  }

  function updateKeyboardLabels() {
    if (!keyboardElement) return;
    var labels = keyboardLabelsForLayout(keyboardLayout);
    var keyLabels = keyboardElement.querySelectorAll(".sf2-key-pc");
    for (var i = 0; i < keyLabels.length; i++) {
      var index = Number(keyLabels[i].dataset.keyIndex);
      if (index >= 0 && index < labels.length) keyLabels[i].textContent = labels[index];
    }
  }

  function setKeyboardLayout(layout, source) {
    if (layout !== "qwertz" && layout !== "qwerty") return;
    keyboardLayout = layout;
    updateKeyboardLabels();
    if (keyboardLayoutInfo) {
      keyboardLayoutInfo.textContent = "Keyboard layout: " +
        (layout === "qwertz" ? "QWERTZ" : "QWERTY") +
        (source === "api" ? " (detected by browser)" : " (detected from key press)");
    }
  }

  function detectKeyboardLayoutFromEvent(event) {
    var key = String(event.key || "").toLowerCase();
    if (event.code === "KeyY") {
      if (key === "z") setKeyboardLayout("qwertz", "event");
      else if (key === "y") setKeyboardLayout("qwerty", "event");
    } else if (event.code === "KeyZ") {
      if (key === "y") setKeyboardLayout("qwertz", "event");
      else if (key === "z") setKeyboardLayout("qwerty", "event");
    }
  }

  function detectKeyboardLayoutFromBrowser() {
    if (keyboardLayoutInfo) {
      keyboardLayoutInfo.textContent =
        "Keyboard layout: press the Z/Y key once to detect it automatically.";
    }
    if (!navigator.keyboard || typeof navigator.keyboard.getLayoutMap !== "function") return;

    try {
      navigator.keyboard.getLayoutMap().then(function (layoutMap) {
        var keyY = String(layoutMap.get("KeyY") || "").toLowerCase();
        var keyZ = String(layoutMap.get("KeyZ") || "").toLowerCase();
        if (keyY === "z" && keyZ === "y") {
          setKeyboardLayout("qwertz", "api");
        } else if (keyY === "y" && keyZ === "z") {
          setKeyboardLayout("qwerty", "api");
        }
      }).catch(function () {
        // Firefox and non-secure local pages may not expose Keyboard.getLayoutMap().
        // The layout is then detected from KeyboardEvent.key on the first Z/Y press.
      });
    } catch (ignore) {
      // Fall back to per-key detection.
    }
  }

  function currentPreset() {
    if (!font) return null;
    return font.presets[parseInt(presetSelect.value, 10)] || null;
  }

  function currentVelocity() {
    return Math.max(1, Math.min(127, parseInt(velocityInput.value, 10) || 100));
  }

  function isWavetableMode() {
    return !!(wavetableModeInput && wavetableModeInput.checked);
  }

  function updateModeInfo() {
    if (!modeInfo) return;
    if (isWavetableMode()) {
      modeInfo.textContent =
        "V2-compatible preview: polyphonic (up to " + V2_MAX_NOTES +
        " held notes), up to " + V2_MAX_LAYERS_PER_NOTE +
        " sample layers per note, 18-bit sample index / 14-bit fractional phase, mono or linked stereo PCM, velocity-scaled level and a maximum of " +
        MAX_SAMPLE_FRAMES_V2.toLocaleString() +
        " sample frames per layer. Note Off exits loop-until-release loops and starts the release envelope. SF2 filters, arbitrary modulators, pan and effects sends are not rendered.";
    } else {
      modeInfo.textContent =
        "Extended browser preview: polyphonic (up to " + MAX_PREVIEW_VOICES +
        " held notes) with up to " + MAX_PREVIEW_LAYERS_PER_NOTE +
        " zones per note. Fixed velocity selection; limited to " + MAX_PREVIEW_SAMPLE_FRAMES.toLocaleString() +
        " frames per sample and " + MAX_PREVIEW_CACHE_FRAMES.toLocaleString() +
        " channel-frames in the sample cache. Linked stereo samples are played in stereo.";
    }
  }

  function updateStopButton() {
    stopButton.disabled = activeVoices.length === 0;
  }

  function updateKeyboardVisual(note, pressed) {
    if (!keyboardElement) return;
    var key = keyboardElement.querySelector('[data-note="' + note + '"]');
    if (key) {
      if (pressed) key.classList.add("is-active");
      else key.classList.remove("is-active");
    }
  }

  function renderKeyboard() {
    if (!keyboardElement || !keyboardStartSelect) return;
    keyboardElement.innerHTML = "";
    pcNoteByCode = Object.create(null);

    var startNote = parseInt(keyboardStartSelect.value, 10);
    if (!Number.isFinite(startNote)) startNote = 48;

    for (var i = 0; i < 24; i++) {
      var note = startNote + i;
      pcNoteByCode[KEYBOARD_PC_CODES[i]] = note;
    }

    // White keys are laid out first; black keys are overlaid at semitone positions.
    for (var w = 0; w < 24; w++) {
      var whitePitch = w % 12;
      if (BLACK_KEY_OFFSETS.indexOf(whitePitch) >= 0) continue;
      var whiteNote = startNote + w;
      var whiteKey = document.createElement("button");
      whiteKey.type = "button";
      whiteKey.className = "sf2-key sf2-white-key";
      whiteKey.dataset.note = String(whiteNote);
      whiteKey.setAttribute("aria-label", midiName(whiteNote) + ", MIDI " + whiteNote);
      whiteKey.disabled = !font;
      var whiteLabel = document.createElement("span");
      whiteLabel.className = "sf2-key-note";
      whiteLabel.textContent = midiName(whiteNote);
      whiteKey.appendChild(whiteLabel);
      var pcWhite = document.createElement("span");
      pcWhite.className = "sf2-key-pc";
      pcWhite.dataset.keyIndex = String(w);
      pcWhite.textContent = keyboardLabelsForLayout(keyboardLayout)[w];
      whiteKey.appendChild(pcWhite);
      keyboardElement.appendChild(whiteKey);
    }

    for (var b = 0; b < 24; b++) {
      var pitchClass = b % 12;
      if (BLACK_KEY_OFFSETS.indexOf(pitchClass) < 0) continue;
      var blackNote = startNote + b;
      var whiteBefore = 0;
      for (var p = 0; p < b; p++) {
        if (BLACK_KEY_OFFSETS.indexOf(p % 12) < 0) whiteBefore++;
      }
      var blackKey = document.createElement("button");
      blackKey.type = "button";
      blackKey.className = "sf2-key sf2-black-key";
      blackKey.dataset.note = String(blackNote);
      blackKey.style.left = "calc(" + (whiteBefore / 14 * 100).toFixed(4) + "% - 2.25%)";
      blackKey.setAttribute("aria-label", midiName(blackNote) + ", MIDI " + blackNote);
      blackKey.disabled = !font;
      var blackLabel = document.createElement("span");
      blackLabel.className = "sf2-key-pc";
      blackLabel.dataset.keyIndex = String(b);
      blackLabel.textContent = keyboardLabelsForLayout(keyboardLayout)[b];
      blackKey.appendChild(blackLabel);
      keyboardElement.appendChild(blackKey);
    }
  }

  function ensureAudioContext() {
    var AudioContextConstructor = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextConstructor) {
      throw new Error("Web Audio is not available in this browser.");
    }
    if (!audioContext) {
      audioContext = new AudioContextConstructor();
      masterGain = audioContext.createGain();
      masterGain.gain.value = 0.7;
      masterGain.connect(audioContext.destination);
    }
    if (audioContext.state === "suspended") {
      return audioContext.resume().then(function () { return audioContext; });
    }
    return Promise.resolve(audioContext);
  }

  function getAudioBuffer(soundFont, region) {
    var layout = makeRegionLayout(soundFont, region);
    if (layout.error) throw new Error(layout.error);
    var cacheKey = region.sampleIndex + ":" + layout.start + ":" + layout.frames +
      ":" + layout.loopStart + ":" + layout.loopEnd;
    if (soundFont.audioBufferCache[cacheKey]) return soundFont.audioBufferCache[cacheKey];

    if (layout.frames > MAX_PREVIEW_SAMPLE_FRAMES) {
      throw new Error("This sample has " + layout.frames.toLocaleString() +
        " frames; the browser preview limit is " +
        MAX_PREVIEW_SAMPLE_FRAMES.toLocaleString() + " frames per zone.");
    }
    var channels = getStereoPcm(soundFont, region);
    var channelCount = channels.stereo ? 2 : 1;
    var cachedFrames = layout.frames * channelCount;
    if (soundFont.audioBufferFrameCount + cachedFrames > MAX_PREVIEW_CACHE_FRAMES) {
      throw new Error("The preview sample cache reached its " +
        MAX_PREVIEW_CACHE_FRAMES.toLocaleString() +
        "-channel-frame memory limit. Load the SoundFont again to clear the preview cache.");
    }
    var buffer = audioContext.createBuffer(channelCount, layout.frames, layout.sampleRate);
    var leftOutput = buffer.getChannelData(0);
    for (var i = 0; i < layout.frames; i++) leftOutput[i] = channels.left[i] / 32768;
    if (channels.stereo) {
      var rightOutput = buffer.getChannelData(1);
      for (var r = 0; r < layout.frames; r++) rightOutput[r] = channels.right[r] / 32768;
    }
    soundFont.audioBufferCache[cacheKey] = buffer;
    soundFont.audioBufferFrameCount += cachedFrames;
    return buffer;
  }

  function envelopeLevelAt(region, note, startTime, time, gainScale) {
    var env = envelopeForNote(region.values, note);
    var attenuation = Math.pow(10, -Math.max(0, region.values[GEN.attenuation]) / 200);
    var sustain = attenuation * Math.pow(10, -env.sustainCb / 200);
    var delayEnd = startTime + env.delay;
    var attackEnd = delayEnd + env.attack;
    var holdEnd = attackEnd + env.hold;
    var decayEnd = holdEnd + env.decay;

    gainScale = gainScale === undefined ? 1 : gainScale;
    if (time <= delayEnd) return 0;
    if (time < attackEnd && env.attack > 0) {
      return attenuation * gainScale * ((time - delayEnd) / env.attack);
    }
    if (time <= holdEnd) return attenuation * gainScale;
    if (time < decayEnd && env.decay > 0) {
      var fraction = (time - holdEnd) / env.decay;
      return (attenuation + (sustain - attenuation) * fraction) * gainScale;
    }
    return sustain * gainScale;
  }

  function removeVoice(voice) {
    var index = activeVoices.indexOf(voice);
    if (index >= 0) activeVoices.splice(index, 1);
    updateStopButton();
  }

  function hardStopVoice(voice) {
    if (!voice || voice.finished) return;
    voice.finished = true;
    for (var i = 0; i < voice.layers.length; i++) {
      var layer = voice.layers[i];
      try { layer.source.stop(); } catch (ignoreStop) {}
      try { layer.source.disconnect(); } catch (ignoreSourceDisconnect) {}
      try { layer.gain.disconnect(); } catch (ignoreGainDisconnect) {}
      layer.ended = true;
    }
    removeVoice(voice);
  }

  function hardStopAllVoices() {
    var voices = activeVoices.slice();
    for (var i = 0; i < voices.length; i++) hardStopVoice(voices[i]);
  }

  function stopPreview() {
    playbackGeneration++;
    hardStopAllVoices();
    keyboardHolds = Object.create(null);
    pointerHolds = Object.create(null);
    pcHolds = Object.create(null);
    if (keyboardElement) {
      var pressed = keyboardElement.querySelectorAll(".sf2-key.is-active");
      for (var i = 0; i < pressed.length; i++) pressed[i].classList.remove("is-active");
    }
    updateStopButton();
  }

  function scheduleGain(gain, region, note, startTime, gainScale) {
    var env = envelopeForNote(region.values, note);
    var sustainGain = Math.pow(10, -env.sustainCb / 200);
    var attenuation = Math.pow(10, -Math.max(0, region.values[GEN.attenuation]) / 200);
    gainScale = gainScale === undefined ? 1 : gainScale;
    var p = gain.gain;
    var t0 = startTime;
    p.setValueAtTime(0, t0);
    var peak = t0 + env.delay + env.attack;
    p.linearRampToValueAtTime(attenuation * gainScale, peak);
    p.setValueAtTime(attenuation * gainScale, peak + env.hold);
    p.linearRampToValueAtTime(attenuation * sustainGain * gainScale, peak + env.hold + env.decay);
  }

  function releaseVoice(voice) {
    if (!voice || voice.released || voice.finished) return;
    voice.released = true;
    var now = audioContext.currentTime;

    for (var i = 0; i < voice.layers.length; i++) {
      var layer = voice.layers[i];
      var env = envelopeForNote(layer.region.values, voice.note);
      var param = layer.gain.gain;
      var level = envelopeLevelAt(layer.region, voice.note, voice.startTime, now, layer.gainScale);
      var endTime = now + Math.max(0, env.release);

      param.cancelScheduledValues(now);
      param.setValueAtTime(level, now);
      if (env.release > 0) param.linearRampToValueAtTime(0, endTime);
      else param.setValueAtTime(0, now);

      // SF2 mode 3 is loop-until-release; V2 exits the loop on Note-Off.
      if (layer.layout.loopMode === 3) {
        layer.source.loop = false;
      }

      try { layer.source.stop(Math.max(now + 0.005, endTime + 0.025)); }
      catch (ignoreStop) {}
    }
  }

  function releaseKeyboardNote(note, sourceId) {
    var holds = keyboardHolds[note];
    if (!holds || !holds[sourceId]) return;
    delete holds[sourceId];
    var remaining = Object.keys(holds).length;
    if (remaining === 0) {
      delete keyboardHolds[note];
      updateKeyboardVisual(note, false);
      for (var i = 0; i < activeVoices.length; i++) {
        if (activeVoices[i].note === note && !activeVoices[i].manual) {
          releaseVoice(activeVoices[i]);
        }
      }
    }
  }

  function holdKeyboardNote(note, sourceId) {
    if (!font) return;
    var holds = keyboardHolds[note];
    if (!holds) holds = keyboardHolds[note] = Object.create(null);
    if (holds[sourceId]) return;
    holds[sourceId] = true;
    updateKeyboardVisual(note, true);

    if (Object.keys(holds).length === 1) {
      startSoundNote(note, currentVelocity(), { manual: false, keyboard: true });
    }
  }

  async function startSoundNote(note, velocity, options) {
    if (!font) return;
    options = options || {};
    var soundFont = font;
    var preset = currentPreset();
    if (!preset) return;
    var startGeneration = playbackGeneration;

    note = Math.max(0, Math.min(127, Math.round(note)));
    velocity = Math.max(1, Math.min(127, Math.round(velocity)));
    setPreviewStatus(presetCompatibilityMessage);
    var wavetableMode = isWavetableMode();
    var matches = regionsForNote(preset, note, velocity);

    if (!matches.length) {
      setPreviewStatus("No SF2 zone matches " + midiName(note) +
        " at velocity " + velocity + ".");
      return;
    }

    var layerLimit = wavetableMode ? V2_MAX_LAYERS_PER_NOTE : MAX_PREVIEW_LAYERS_PER_NOTE;
    if (matches.length > layerLimit) {
      setPreviewStatus("Preview refused: " + matches.length +
        " zones match this note; the selected preview limit is " +
        layerLimit + " layers per note.");
      return;
    }

    if (wavetableMode) {
      for (var compatibleIndex = 0; compatibleIndex < matches.length; compatibleIndex++) {
        var compatibleLayout = makeRegionLayout(font, matches[compatibleIndex]);
        if (compatibleLayout.error) {
          setPreviewStatus("Preview failed: " + compatibleLayout.error);
          return;
        }
        if (compatibleLayout.frames > MAX_SAMPLE_FRAMES_V2) {
          setPreviewStatus("V2-compatible preview refused: this sample has " +
            compatibleLayout.frames.toLocaleString() +
            " frames; the V2 18-bit sample index supports at most " +
            MAX_SAMPLE_FRAMES_V2 + ".");
          return;
        }
      }
    }

    var maximumPreviewVoices = wavetableMode ? V2_MAX_NOTES : MAX_PREVIEW_VOICES;
    var unReleasedVoices = activeVoices.filter(function (voice) { return !voice.released; });
    if (unReleasedVoices.length >= maximumPreviewVoices && unReleasedVoices.length) {
      releaseVoice(unReleasedVoices[0]);
    }

    try {
      await ensureAudioContext();
      if (startGeneration !== playbackGeneration || soundFont !== font) return;
      if (options.keyboard && !keyboardHolds[note]) return;

      var startTime = audioContext.currentTime + 0.015;
      var voice = {
        note: note,
        velocity: velocity,
        startTime: startTime,
        wavetableMode: wavetableMode,
        manual: !!options.manual,
        keyboard: !!options.keyboard,
        released: false,
        finished: false,
        layers: []
      };
      activeVoices.push(voice);

      try {
        for (var i = 0; i < matches.length; i++) {
          var region = matches[i];
          var layout = makeRegionLayout(font, region);
          if (layout.error) throw new Error(layout.error);

          var source = audioContext.createBufferSource();
          var gain = audioContext.createGain();
          source.buffer = getAudioBuffer(soundFont, region);
          source.playbackRate.value = playbackRatio(font, region, note);
          source.loop = layout.loop;
          if (source.loop) {
            source.loopStart = layout.loopStart / layout.sampleRate;
            source.loopEnd = layout.loopEnd / layout.sampleRate;
          }

          var gainScale = wavetableMode ? (velocity / 127) : 1;
          scheduleGain(gain, region, note, startTime, gainScale);
          source.connect(gain);
          gain.connect(masterGain);

          var layer = {
            source: source,
            gain: gain,
            region: region,
            layout: layout,
            gainScale: gainScale,
            ended: false
          };
          voice.layers.push(layer);
          source.onended = function (currentLayer) {
            return function () {
              currentLayer.ended = true;
              try { currentLayer.source.disconnect(); } catch (ignoreSourceDisconnect) {}
              try { currentLayer.gain.disconnect(); } catch (ignoreGainDisconnect) {}
              if (voice.finished) return;
              var allEnded = voice.layers.every(function (item) { return item.ended; });
              if (allEnded) {
                voice.finished = true;
                removeVoice(voice);
              }
            };
          }(layer);
          source.start(startTime);
        }
      } catch (layerError) {
        hardStopVoice(voice);
        throw layerError;
      }

      updateStopButton();
      setStatus("Playing " + midiName(note) + " at velocity " + velocity +
        " using " + matches.length + " zone(s)." +
        (wavetableMode ? " V2-compatible mode." : " Extended browser mode."), "good");
    } catch (error) {
      setPreviewStatus("Preview failed: " + error.message);
    }
  }

  async function playPreview() {
    if (!font) return;
    stopPreview();
    var note = Math.max(0, Math.min(127, parseInt(noteInput.value, 10) || 60));
    await startSoundNote(note, currentVelocity(), { manual: true, keyboard: false });
  }

  function clearHeldNotes() {
    var notes = Object.keys(keyboardHolds);
    for (var i = 0; i < notes.length; i++) {
      var note = Number(notes[i]);
      var voices = activeVoices.slice();
      for (var j = 0; j < voices.length; j++) {
        if (voices[j].note === note && !voices[j].manual) releaseVoice(voices[j]);
      }
      updateKeyboardVisual(note, false);
    }
    keyboardHolds = Object.create(null);
    pointerHolds = Object.create(null);
    pcHolds = Object.create(null);
  }

  function isTypingTarget(target) {
    if (!target || !target.closest) return false;
    // Ignore only actual text-entry controls. File inputs, checkboxes and
    // selectors often retain focus after use and must not block note keys.
    return !!target.closest(
      "textarea, input[type='text'], input[type='number'], input[type='search'], " +
      "input[type='email'], input[type='url'], input[type='tel'], input[type='password'], " +
      "[contenteditable='true']"
    );
  }

  function numberArray(values, perLine) {
    var lines = [];
    for (var i = 0; i < values.length; i += perLine) {
      lines.push("    " + values.slice(i, i + perLine).join(", "));
    }
    return lines.join(",\n");
  }

  function identifierForSample(prefix, index) {
    return prefix + "Pcm" + index;
  }

  function countExpression(seconds) {
    if (!seconds || seconds <= 0) return "0U";
    return "((uint32_t)(AUDIO_SAMPLE_RATE_EXACT * " + seconds.toFixed(9) +
      "f / AudioSynthWavetable::ENVELOPE_PERIOD + 0.5f))";
  }

  function buildSampleData(soundFont, region, note, sampleName) {
    var layout = makeRegionLayout(soundFont, region);
    var sample = soundFont.samples[region.sampleIndex];
    var values = region.values;
    var root = rootKeyForRegion(soundFont, region);
    var playback = playbackRatio(soundFont, region, note);
    var phase = 4294967296.0 * layout.sampleRate * playback /
      (Math.pow(2, V1_INDEX_BITS) * midiFrequency(note));
    var env = envelopeForNote(values, note);
    var attenuation = Math.max(0, Math.min(1440, values[GEN.attenuation]));
    var initialScalar = Math.round(65535 * Math.pow(10, -attenuation / 200));
    var sustainGain = Math.pow(10, -env.sustainCb / 200);
    var sustainMultiplier = Math.round(2147483647 * (1 - sustainGain));
    var maxPhase = layout.frames * Math.pow(2, 32 - V1_INDEX_BITS);
    var loopEnd = layout.loop ? layout.loopEnd * Math.pow(2, 32 - V1_INDEX_BITS) : 0;
    var loopLength = layout.loop ?
      (layout.loopEnd - layout.loopStart) * Math.pow(2, 32 - V1_INDEX_BITS) : 0;
    if (!Number.isSafeInteger(maxPhase) || maxPhase > 0xffffffff ||
        loopEnd > 0xffffffff || loopLength > 0xffffffff) {
      throw new Error("Sample phase range exceeds the current AudioSynthWavetable limits.");
    }

    return {
      sampleName: sampleName,
      samplePointer: sampleName,
      loop: layout.loop,
      phaseIncrement: phase,
      maxPhase: maxPhase,
      loopEnd: loopEnd,
      loopLength: loopLength,
      initialScalar: initialScalar,
      delay: env.delay,
      attack: env.attack,
      hold: env.hold,
      decay: env.decay,
      release: env.release,
      sustainMultiplier: sustainMultiplier,
      rootKey: root,
      sampleRate: layout.sampleRate,
      loopMode: layout.loopMode
    };
  }

  function countExpressionV2(seconds) {
    if (!seconds || seconds <= 0) return "0U";
    return "((uint32_t)(AUDIO_SAMPLE_RATE_EXACT * " + seconds.toFixed(9) +
      "f / AudioSynthWavetableV2::ENVELOPE_PERIOD + 0.5f))";
  }

  function buildSampleDataV2(soundFont, region, note, pcmEntry) {
    var layout = makeRegionLayout(soundFont, region);
    var sample = soundFont.samples[region.sampleIndex];
    var values = region.values;
    var playback = playbackRatio(soundFont, region, note);
    // V2 multiplies this per-Hz coefficient by MIDI-note frequency and
    // adapts it to the actual runtime output rate in setInstrument().
    var phaseCoefficient = V2_PHASE_SCALE *
      layout.sampleRate * playback / midiFrequency(note);
    var env = envelopeForNote(values, note);
    var attenuation = Math.max(0, Math.min(1440, values[GEN.attenuation]));
    var initialScalar = Math.round(65535 * Math.pow(10, -attenuation / 200));
    var sustainGain = Math.pow(10, -env.sustainCb / 200);
    var sustainMultiplier = Math.round(2147483647 * (1 - sustainGain));
    var maxPhase = layout.frames * V2_PHASE_SCALE;
    var loopEnd = layout.loop ? layout.loopEnd * V2_PHASE_SCALE : 0;
    var loopLength = layout.loop ?
      (layout.loopEnd - layout.loopStart) * V2_PHASE_SCALE : 0;

    if (!Number.isFinite(phaseCoefficient) || phaseCoefficient <= 0 ||
        !Number.isSafeInteger(maxPhase) || maxPhase > 0xffffffff ||
        !Number.isSafeInteger(loopEnd) || loopEnd > 0xffffffff ||
        !Number.isSafeInteger(loopLength) || loopLength > 0xffffffff) {
      throw new Error("A sample phase or loop range exceeds the V2 32-bit phase limits.");
    }

    var loopMode = "ONE_SHOT";
    if (layout.loop && layout.loopMode === 1) loopMode = "CONTINUOUS";
    else if (layout.loop && layout.loopMode === 3) loopMode = "UNTIL_RELEASE";

    return {
      pcmName: pcmEntry.leftName,
      rightPcmName: pcmEntry.rightName,
      sampleCount: layout.frames + 1,
      phaseCoefficient: phaseCoefficient,
      maxPhase: maxPhase,
      loopMode: loopMode,
      loopEnd: loopEnd,
      loopLength: loopLength,
      initialScalar: initialScalar,
      delay: env.delay,
      attack: env.attack,
      hold: env.hold,
      decay: env.decay,
      release: env.release,
      sustainMultiplier: sustainMultiplier,
      sampleRate: sample.sampleRate
    };
  }

  async function generateV2Header() {
    var preset = currentPreset();
    if (!preset) throw new Error("Select a preset first.");
    if (!preset.zones.length) throw new Error("The selected preset contains no playable sample zones.");

    var prefix = safeIdentifier(nameInput.value || preset.name);
    var sampleMap = Object.create(null);
    var sampleEntries = [];
    var descriptorEntries = [];
    var zoneEntries = [];
    var sampleFrameTotal = 0;
    var stereoPairCount = 0;
    var featureWarningMap = Object.create(null);
    var firstLayerOverflow = null;

    // V2 can play at most four matching layers for a note/velocity pair.
    // Check the complete MIDI domain so no active zone is silently discarded.
    for (var testNote = 0; testNote < 128; testNote++) {
      for (var testVelocity = 1; testVelocity <= 127; testVelocity++) {
        var testMatches = regionsForNote(preset, testNote, testVelocity);
        if (testMatches.length > V2_MAX_LAYERS_PER_NOTE) {
          firstLayerOverflow = {
            note: testNote,
            velocity: testVelocity,
            layers: testMatches.length
          };
          break;
        }
      }
      if (firstLayerOverflow) break;
      if ((testNote & 7) === 7) {
        await new Promise(function (resolve) { setTimeout(resolve, 0); });
      }
    }

    if (firstLayerOverflow) {
      throw new Error("V2 supports at most " + V2_MAX_LAYERS_PER_NOTE +
        " sample layers per note. " + midiName(firstLayerOverflow.note) +
        " at velocity " + firstLayerOverflow.velocity + " has " +
        firstLayerOverflow.layers + " matching zones. Reduce the preset's layers or choose another preset.");
    }

    for (var r = 0; r < preset.zones.length; r++) {
      var region = preset.zones[r];
      var layout = makeRegionLayout(font, region);
      if (layout.error) {
        throw new Error("Zone " + (r + 1) + " (" + region.instrumentName + "): " + layout.error);
      }
      if (layout.frames > MAX_SAMPLE_FRAMES_V2) {
        throw new Error("Zone " + (r + 1) + " uses " + layout.frames +
          " frames; V2 supports at most " + MAX_SAMPLE_FRAMES_V2 +
          " frames per layer with the fixed 18/14 phase format.");
      }
      if (layout.loopMode === 2) {
        featureWarningMap.reservedSampleMode = "SF2 sampleModes value 2 is reserved/unsupported and was exported as one-shot audio.";
      }
      if ((layout.loopMode === 1 || layout.loopMode === 3) && !layout.loop) {
        featureWarningMap.invalidLoop = "One or more zones have invalid forward-loop points and were exported as one-shot samples.";
      }
      if (region.values[GEN.velocity] >= 0) {
        featureWarningMap.fixedVelocity = "One or more zones use the SF2 fixed-velocity generator; V2 uses the incoming MIDI velocity instead.";
      }

      var pcmGuardFrame = layout.loop && layout.loopEnd === layout.frames ? layout.loopStart : -1;
      var pcmKey = (layout.right ? "stereo:" : "mono:") + layout.start + ":" +
        layout.rightStart + ":" + layout.frames + ":" + pcmGuardFrame;
      var pcmEntry = sampleMap[pcmKey];
      if (!pcmEntry) {
        var channels = getStereoPcm(font, region);
        var frameCost = layout.frames * (channels.stereo ? 2 : 1);
        if (sampleFrameTotal + frameCost > MAX_EXPORT_FRAMES) {
          throw new Error("The preset exceeds the V2 export limit of " +
            MAX_EXPORT_FRAMES.toLocaleString() + " unique channel-frames.");
        }
        pcmEntry = {
          left: channels.left,
          right: channels.right,
          leftName: identifierForSample(prefix, sampleEntries.length) + (channels.stereo ? "L" : ""),
          rightName: channels.stereo ? identifierForSample(prefix, sampleEntries.length) + "R" : null,
          frames: layout.frames,
          stereo: channels.stereo
        };
        sampleMap[pcmKey] = pcmEntry;
        sampleEntries.push(pcmEntry);
        sampleFrameTotal += frameCost;
        if (channels.stereo) stereoPairCount++;
      }

      // One descriptor can cover a complete key range with standard tuning.
      // Split to single-key zones only when pitch or envelope key tracking
      // makes per-note phase/envelope data necessary.
      var values = region.values;
      var splitByKey = values[GEN.keynum] >= 0 ||
        values[GEN.scaleTuning] !== 100 ||
        values[GEN.keyToVolHold] !== 0 ||
        values[GEN.keyToVolDecay] !== 0;
      var keyFirst = region.keyRange.low;
      var keyLast = region.keyRange.high;
      var descriptorCountForZone = splitByKey ? (keyLast - keyFirst + 1) : 1;

      for (var keyIndex = 0; keyIndex < descriptorCountForZone; keyIndex++) {
        var note = splitByKey ? keyFirst + keyIndex : keyFirst;
        var sampleData = buildSampleDataV2(font, region, note, pcmEntry);
        var descriptorIndex = descriptorEntries.length;
        descriptorEntries.push(sampleData);
        zoneEntries.push({
          keyLow: splitByKey ? note : keyFirst,
          keyHigh: splitByKey ? note : keyLast,
          velocityLow: region.velocityRange.low,
          velocityHigh: region.velocityRange.high,
          sampleIndex: descriptorIndex
        });

        if (zoneEntries.length > MAX_EXPORT_ZONES ||
            descriptorEntries.length > MAX_EXPORT_ZONES) {
          throw new Error("The preset expands to more than " +
            MAX_EXPORT_ZONES.toLocaleString() +
            " V2 zone/sample descriptors. Select a smaller preset.");
        }
      }

      if ((r & 15) === 15) {
        await new Promise(function (resolve) { setTimeout(resolve, 0); });
      }
    }

    if (!zoneEntries.length) throw new Error("The selected preset has no valid exportable zones.");

    var lines = [];
    lines.push("// Generated by ESP-beatsy Audio Tools - SF2 Converter V2.");
    lines.push("// Source: " + currentFileName.replace(/[\r\n]/g, " ").replace(/\*\//g, "* /"));
    lines.push("// Preset: " + preset.name.replace(/[\r\n]/g, " "));
    lines.push("// Bank " + preset.bank + ", program " + preset.program + ".");
    lines.push("// Key and velocity zones are retained; overlapping zones become simultaneous sample layers.");
    lines.push("// Linked stereo sample pairs are preserved as separate left/right PCM arrays.");
    lines.push("// Requires AudioSynthWavetableV2 (fixed 18/14 phase format).");
    lines.push("// Legacy AudioSynthWavetable data is not modified.");
    lines.push("// PCM arrays contain one interpolation guard frame per channel.");
    lines.push("// All SF2 sample rates are retained; V2 adapts playback to the runtime audio sample rate.");
    lines.push("// SF2 filters, arbitrary modulators, pan/placement, chorus/reverb sends and reserved sample modes are not rendered.");
    if (font.modulatorCount) lines.push("// WARNING: " + font.modulatorCount + " SF2 modulator record(s) were ignored.");
    if (preset.warnings.length) lines.push("// WARNING: unsupported SF2 generators are present; see the tool's preset report.");
    Object.keys(featureWarningMap).forEach(function (key) {
      lines.push("// WARNING: " + featureWarningMap[key]);
    });
    if (font.sample24Chunk) lines.push("// INFO: optional sm24 low bytes were rounded into signed 16-bit PCM.");
    lines.push("#pragma once");
    lines.push("#include <Audio.h>");
    lines.push("#include <stdint.h>");
    lines.push("");

    for (var a = 0; a < sampleEntries.length; a++) {
      var arrays = [{ name: sampleEntries[a].leftName, pcm: sampleEntries[a].left }];
      if (sampleEntries[a].stereo) {
        arrays.push({ name: sampleEntries[a].rightName, pcm: sampleEntries[a].right });
      }
      for (var c = 0; c < arrays.length; c++) {
        lines.push("static const int16_t " + arrays[c].name + "[" +
          arrays[c].pcm.length + "] PROGMEM = {");
        var valuesPerLine = [];
        for (var v = 0; v < arrays[c].pcm.length; v++) {
          valuesPerLine.push(String(arrays[c].pcm[v]));
          if (valuesPerLine.length === 16 || v === arrays[c].pcm.length - 1) {
            lines.push("    " + valuesPerLine.join(",") +
              (v === arrays[c].pcm.length - 1 ? "" : ","));
            valuesPerLine = [];
          }
          if ((v & 16383) === 16383) {
            await new Promise(function (resolve) { setTimeout(resolve, 0); });
          }
        }
        lines.push("};");
        lines.push("");
      }
    }

    lines.push("static const AudioSynthWavetableV2::sample_data " +
      prefix + "Samples[" + descriptorEntries.length + "] = {");
    for (var d = 0; d < descriptorEntries.length; d++) {
      var item = descriptorEntries[d];
      lines.push("    { " + item.pcmName + ", " + item.sampleCount + "U, " +
        item.phaseCoefficient.toFixed(12) +
        "f / AUDIO_SAMPLE_RATE_EXACT, " + item.maxPhase + "U, " +
        "AudioSynthWavetableV2::LoopMode::" + item.loopMode + ", " +
        item.loopEnd + "U, " + item.loopLength + "U, " +
        item.initialScalar + "U, " +
        countExpressionV2(item.delay) + ", " +
        countExpressionV2(item.attack) + ", " +
        countExpressionV2(item.hold) + ", " +
        countExpressionV2(item.decay) + ", " +
        countExpressionV2(item.release) + ", " +
        item.sustainMultiplier + ", " +
        (item.rightPcmName || "nullptr") + " }" +
        (d === descriptorEntries.length - 1 ? "" : ","));
    }
    lines.push("};");
    lines.push("");

    lines.push("static const AudioSynthWavetableV2::zone_data " +
      prefix + "Zones[" + zoneEntries.length + "] = {");
    for (var z = 0; z < zoneEntries.length; z++) {
      var zone = zoneEntries[z];
      lines.push("    { " + zone.keyLow + ", " + zone.keyHigh + ", " +
        zone.velocityLow + ", " + zone.velocityHigh + ", " +
        prefix + "Samples[" + zone.sampleIndex + "] }" +
        (z === zoneEntries.length - 1 ? "" : ","));
    }
    lines.push("};");
    lines.push("");
    lines.push("static const AudioSynthWavetableV2::instrument_data " +
      prefix + "Instrument(" + prefix + "Zones, " + zoneEntries.length + "U);");
    lines.push("");
    lines.push("// Usage:");
    lines.push("// AudioSynthWavetableV2 player;");
    lines.push("// if (!player.setInstrument(" + prefix + "Instrument)) { /* invalid data */ }");
    lines.push("// player.noteOn(60, 100);");
    lines.push("// player.noteOff(60);");
    lines.push("// Mono layers are mirrored to output 0/1; stereo layers retain both channels.");
    lines.push("// The V2 engine plays up to four matching sample layers per note.");
    lines.push("");

    return {
      code: lines.join("\n"),
      fileName: prefix + "_V2.h",
      zoneCount: zoneEntries.length,
      sampleCount: sampleEntries.length,
      descriptorCount: descriptorEntries.length,
      uniqueFrames: sampleFrameTotal,
      stereoPairs: stereoPairCount,
      mappedNotes: zoneEntries.reduce(function (count, zone) {
        return count + (zone.keyLow === zone.keyHigh ? 1 : zone.keyHigh - zone.keyLow + 1);
      }, 0)
    };
  }

  async function generateHeader() {
    var preset = font.presets[parseInt(presetSelect.value, 10)];
    if (!preset) throw new Error("Select a preset first.");
    var velocity = Math.max(1, Math.min(127, parseInt(velocityInput.value, 10) || 100));
    var prefix = safeIdentifier(nameInput.value || preset.name);
    var matchesByNote = [];
    var noteErrors = [];
    var sampleMap = Object.create(null);
    var sampleEntries = [];
    var sampleFrameTotal = 0;
    var noteData = new Array(128);

    for (var note = 0; note < 128; note++) {
      var matches = regionsForNote(preset, note, velocity);
      if (matches.length > 1) {
        noteErrors.push(midiName(note) + " has " + matches.length + " overlapping sample zones");
      } else if (matches.length === 1) {
        var region = matches[0];
        var layout = makeRegionLayout(font, region);
        if (layout.error) {
          noteErrors.push(midiName(note) + ": " + layout.error);
        } else if (layout.frames > MAX_SAMPLE_FRAMES_V1) {
          noteErrors.push(midiName(note) + " uses " + layout.frames +
            " sample frames; the legacy V1 wavetable index is limited to " + MAX_SAMPLE_FRAMES_V1);
        } else {
          var sampleKey = region.sampleIndex + ":" + layout.start + ":" + layout.frames +
            ":" + layout.loopStart + ":" + layout.loopEnd;
          if (!sampleMap[sampleKey]) {
            var pcm = getPcm(font, region);
            sampleFrameTotal += layout.frames;
            if (sampleFrameTotal > MAX_EXPORT_FRAMES) {
              throw new Error("The selected preset exceeds the V1 export limit of " +
                MAX_EXPORT_FRAMES.toLocaleString() + " unique PCM frames.");
            }
            var sampleName = identifierForSample(prefix, sampleEntries.length);
            sampleMap[sampleKey] = sampleName;
            sampleEntries.push({ name: sampleName, pcm: pcm, layout: layout });
          }
          var sampleData = buildSampleData(font, region, note, sampleMap[sampleKey]);
          noteData[note] = sampleData;
        }
      }
      if ((note & 15) === 15) await new Promise(function (resolve) { setTimeout(resolve, 0); });
    }

    if (noteErrors.length) {
      throw new Error("V1 cannot export this preset at velocity " + velocity +
        " because the existing wavetable API cannot represent every active key zone:\n" +
        noteErrors.slice(0, 12).join("\n") +
        (noteErrors.length > 12 ? "\n… and " + (noteErrors.length - 12) + " more." : "") +
        "\nChoose another velocity, or use a simpler preset without overlapping layers.");
    }

    var lines = [];
    lines.push("// Generated by ESP-beatsy Audio Tools - SF2 Converter V1.");
    lines.push("// Source: " + currentFileName.replace(/[\r\n]/g, " ").replace(/\*\//g, "* /"));
    lines.push("// Preset: " + preset.name.replace(/[\r\n]/g, " "));
    lines.push("// Bank " + preset.bank + ", program " + preset.program + ", baked velocity " + velocity + ".");
    lines.push("// V1 supports one sample zone per MIDI note. Velocity selection is baked into this header.");
    lines.push("// Requires the legacy AudioSynthWavetable::instrument_data interface.");
    lines.push("// SF2 modulators, filters, pan, chorus/reverb sends and stereo placement are not rendered.");
    if (font.modulatorCount) lines.push("// WARNING: " + font.modulatorCount + " SF2 modulator record(s) were ignored.");
    if (preset.warnings.length) lines.push("// WARNING: unsupported SF2 generators are present; see the tool's preset report.");
    lines.push("#pragma once");
    lines.push("#include <Audio.h>");
    lines.push("#include <stdint.h>");
    lines.push("");

    for (var a = 0; a < sampleEntries.length; a++) {
      lines.push("static const int16_t " + sampleEntries[a].name + "[" +
        sampleEntries[a].pcm.length + "] PROGMEM = {");
      var values = [];
      for (var v = 0; v < sampleEntries[a].pcm.length; v++) {
        values.push(String(sampleEntries[a].pcm[v]));
        if (values.length === 16 || v === sampleEntries[a].pcm.length - 1) {
          lines.push("    " + values.join(",") + (v === sampleEntries[a].pcm.length - 1 ? "" : ","));
          values = [];
        }
        if ((v & 16383) === 16383) await new Promise(function (resolve) { setTimeout(resolve, 0); });
      }
      lines.push("};");
      lines.push("");
    }

    lines.push("static const uint8_t " + prefix + "NoteRanges[128] = {");
    lines.push(numberArray(Array.from({ length: 128 }, function (_, index) { return index; }), 16));
    lines.push("};");
    lines.push("");
    lines.push("static const AudioSynthWavetable::sample_data " + prefix + "Samples[128] = {");

    for (var n = 0; n < 128; n++) {
      var item = noteData[n];
      if (!item) {
        lines.push("    { nullptr, false, 16, 0.0f, 0U, 0U, 0U, 0U,");
        lines.push("      0U, 0U, 0U, 0U, 0U, 0,");
        lines.push("      UINT32_MAX, 0U, 0.0f, 0.0f, UINT32_MAX, 0U, 0.0f, 0.0f, 0, 0 }" +
          (n === 127 ? "" : ","));
        continue;
      }
      lines.push("    { " + item.samplePointer + ", " + (item.loop ? "true" : "false") +
        ", 16, " + item.phaseIncrement.toFixed(9) + "f / AUDIO_SAMPLE_RATE_EXACT, " +
        item.maxPhase + "U, " + item.loopEnd + "U, " + item.loopLength + "U, " +
        item.initialScalar + "U,");
      lines.push("      " + countExpression(item.delay) + ", " +
        countExpression(item.attack) + ", " + countExpression(item.hold) + ", " +
        countExpression(item.decay) + ", " + countExpression(item.release) + ", " +
        item.sustainMultiplier + ",");
      lines.push("      UINT32_MAX, 0U, 0.0f, 0.0f, UINT32_MAX, 0U, 0.0f, 0.0f, 0, 0 }" +
        (n === 127 ? "" : ","));
    }

    lines.push("};");
    lines.push("");
    lines.push("static const AudioSynthWavetable::instrument_data " + prefix + "Instrument = {");
    lines.push("    128, " + prefix + "NoteRanges, " + prefix + "Samples");
    lines.push("};");
    lines.push("");
    lines.push("// Usage:");
    lines.push("// AudioSynthWavetable player;");
    lines.push("// player.setInstrument(" + prefix + "Instrument);");
    lines.push("// player.playNote(60, AudioSynthWavetable::DEFAULT_AMPLITUDE);");
    lines.push("");
    lines.push("// V1 limitations: one active sample per key at the chosen velocity;");
    lines.push("// loop-until-release zones continue looping during the release envelope.");

    return {
      code: lines.join("\n"),
      fileName: prefix + ".h",
      velocity: velocity,
      preset: preset,
      sampleCount: sampleEntries.length,
      uniqueFrames: sampleFrameTotal,
      mappedNotes: noteData.filter(Boolean).length
    };
  }

  function downloadText(content, fileName) {
    var blob = new Blob([content], { type: "text/plain;charset=utf-8" });
    var url = URL.createObjectURL(blob);
    var anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = fileName;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
  }

  function resetOutput() {
    generatedCode = "";
    generatedFileName = "";
    codeOutput.value = "";
    codePanel.classList.add("hidden");
    outputInfo.textContent = "";
    downloadButton.disabled = true;
    copyButton.disabled = true;
  }

  fileInput.addEventListener("change", async function () {
    // A new selection invalidates every older asynchronous file read.
    // Large SF2 files can finish reading out of order.
    var thisLoad = ++loadGeneration;

    resetAllPresetScan();
    stopPreview();
    resetOutput();
    clearPreviewStatus();
    presetCompatibilityMessage = "";
    font = null;
    currentFileName = "";
    presetSelect.innerHTML = "";
    presetSelect.disabled = true;
    presetInfo.textContent = "";
    if (analyzerPanel) analyzerPanel.textContent = "";
    metadata.innerHTML = "";
    summary.textContent = "";
    renderKeyboard();
    exportButton.disabled = true;
    playButton.disabled = true;
    stopButton.disabled = true;

    var file = fileInput.files && fileInput.files[0];
    if (!file) {
      setStatus("", "");
      return;
    }
    if (file.size > MAX_FILE_BYTES) {
      setStatus("The selected file exceeds the 512 MiB safety limit.", "bad");
      return;
    }

    currentFileName = file.name;
    setStatus("Reading and indexing " + file.name + "…", "");
    try {
      var buffer = await file.arrayBuffer();

      // Do not let a previous selection overwrite the latest one.
      if (thisLoad !== loadGeneration) return;

      var parsedFont = parseSoundFont(buffer);
      if (thisLoad !== loadGeneration) return;
      if (!parsedFont.presets.length) throw new Error("No playable presets were found.");

      font = parsedFont;
      renderKeyboard();
      populatePresets();
      renderMetadata(font.info);
      var zoneCount = font.presets.reduce(function (sum, preset) {
        return sum + preset.zones.length;
      }, 0);
      summary.textContent = "Loaded file: " + file.name + " · " +
        (font.info.INAM ? "SoundFont: " + font.info.INAM + " · " : "") +
        font.presets.length + " presets · " + font.instrumentCount +
        " instruments · " + (font.samples.length - 1) + " samples · " +
        font.sampleCount.toLocaleString() + " sample points · " +
        zoneCount + " resolved zones.";
      if (font.sample24Chunk) summary.textContent += " 24-bit sample data found.";

      presetSelect.selectedIndex = 0;
      updatePresetInfo();
      setStatus("Loaded " + file.name + ". The first preset is selected; choose another preset in the list to audition it.", "good");
      exportButton.disabled = false;
      playButton.disabled = false;
      allPresetScanButton.disabled = false;
    } catch (error) {
      if (thisLoad !== loadGeneration) return;
      font = null;
      presetCompatibilityMessage = "";
      currentFileName = "";
      presetSelect.innerHTML = "";
      presetSelect.disabled = true;
      metadata.innerHTML = "";
      summary.textContent = "";
      if (analyzerPanel) analyzerPanel.textContent = "";
      renderKeyboard();
      exportButton.disabled = true;
      playButton.disabled = true;
      setStatus("Could not load SoundFont: " + error.message, "bad");
    }
  });

  presetSelect.addEventListener("change", function () {
    stopPreview();
    resetOutput();
    clearPreviewStatus();
    updatePresetInfo();
  });
  if (exportVersionSelect) {
    exportVersionSelect.addEventListener("change", function () {
      resetOutput();
    });
  }
  velocityInput.addEventListener("change", function () {
    stopPreview();
    resetOutput();
    clearPreviewStatus();
    updatePresetInfo();
  });
  noteInput.addEventListener("change", function () {
    updatePresetInfo();
  });
  playButton.addEventListener("click", playPreview);
  stopButton.addEventListener("click", stopPreview);

  allPresetScanButton.addEventListener("click", scanAllSoundFontPresets);
  allPresetScanCopyButton.addEventListener("click", async function () {
    if (!allPresetScanLog.value) return;
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(allPresetScanLog.value);
      } else {
        allPresetScanLog.focus();
        allPresetScanLog.select();
        if (!document.execCommand("copy")) throw new Error("Clipboard access was blocked.");
      }
      allPresetScanStatus.textContent = "Full preset report copied to the clipboard.";
    } catch (error) {
      allPresetScanLog.focus();
      allPresetScanLog.select();
      allPresetScanStatus.textContent = "Clipboard access was blocked. The report is selected; use Ctrl+C.";
    }
  });
  allPresetScanDownloadButton.addEventListener("click", function () {
    if (!allPresetScanLog.value) return;
    var baseName = currentFileName.replace(/\.[^.]+$/, "").replace(/[^A-Za-z0-9_-]/g, "_");
    downloadText(allPresetScanLog.value, (baseName || "SoundFont") + "_preset_voice_analysis.txt");
  });

  if (keyboardElement) {
    renderKeyboard();

    keyboardElement.addEventListener("pointerdown", function (event) {
      var key = event.target.closest(".sf2-key");
      if (!key || !keyboardElement.contains(key)) return;
      event.preventDefault();
      var note = Number(key.dataset.note);
      var pointerId = String(event.pointerId);
      if (pointerHolds[pointerId] !== undefined) return;
      pointerHolds[pointerId] = note;
      holdKeyboardNote(note, "pointer:" + pointerId);
    });

    window.addEventListener("pointerup", function (event) {
      var pointerId = String(event.pointerId);
      if (pointerHolds[pointerId] === undefined) return;
      var note = pointerHolds[pointerId];
      delete pointerHolds[pointerId];
      releaseKeyboardNote(note, "pointer:" + pointerId);
    });
    window.addEventListener("pointercancel", function (event) {
      var pointerId = String(event.pointerId);
      if (pointerHolds[pointerId] === undefined) return;
      var note = pointerHolds[pointerId];
      delete pointerHolds[pointerId];
      releaseKeyboardNote(note, "pointer:" + pointerId);
    });
  }

  document.addEventListener("keydown", function (event) {
    if (event.repeat || event.isComposing || event.ctrlKey || event.altKey ||
        event.metaKey || isTypingTarget(event.target)) return;

    detectKeyboardLayoutFromEvent(event);
    var note = pcNoteByCode[event.code];
    if (note === undefined) return;

    event.preventDefault();
    var sourceId = "pc:" + event.code;
    if (pcHolds[event.code]) return;
    pcHolds[event.code] = true;
    holdKeyboardNote(note, sourceId);
  }, true);

  document.addEventListener("keyup", function (event) {
    if (!pcHolds[event.code]) return;
    var note = pcNoteByCode[event.code];
    delete pcHolds[event.code];
    if (note !== undefined) releaseKeyboardNote(note, "pc:" + event.code);
  }, true);

  window.addEventListener("blur", clearHeldNotes);
  document.addEventListener("visibilitychange", function () {
    if (document.hidden) clearHeldNotes();
  });

  if (keyboardStartSelect) {
    keyboardStartSelect.addEventListener("change", function () {
      stopPreview();
      renderKeyboard();
    });
  }
  if (wavetableModeInput) {
    wavetableModeInput.addEventListener("change", function () {
      stopPreview();
      clearPreviewStatus();
      updateModeInfo();
      updatePresetInfo();
    });
  }
  exportButton.addEventListener("click", async function () {
    if (!font) return;
    stopPreview();
    exportButton.disabled = true;
    resetOutput();
    setStatus("Resolving zones and generating the instrument header…", "");
    try {
      var selectedVersion = exportVersionSelect ? exportVersionSelect.value : "v2";
      var result = selectedVersion === "v1" ? await generateHeader() : await generateV2Header();
      generatedCode = result.code;
      generatedFileName = result.fileName;
      codeOutput.value = generatedCode;
      codePanel.classList.remove("hidden");
      downloadButton.disabled = false;
      copyButton.disabled = false;
      if (selectedVersion === "v1") {
        outputInfo.textContent = "Generated legacy V1 header · " +
          result.mappedNotes + " mapped notes · " + result.sampleCount +
          " unique mono sample arrays · " + result.uniqueFrames.toLocaleString() +
          " PCM frames · baked velocity " + result.velocity + ".";
      } else {
        outputInfo.textContent = "Generated V2 header · " +
          result.zoneCount + " key/velocity zones · " + result.descriptorCount +
          " sample descriptors · " + result.sampleCount + " unique PCM sample regions · " +
          result.uniqueFrames.toLocaleString() + " channel-frames · " +
          result.stereoPairs + " stereo sample pairs.";
      }
      setStatus("Instrument header generated. Add it to the sketch and call setInstrument().", "good");
    } catch (error) {
      setStatus("Export not possible: " + error.message, "bad");
    } finally {
      exportButton.disabled = false;
    }
  });

  downloadButton.addEventListener("click", function () {
    if (generatedCode) downloadText(generatedCode, generatedFileName);
  });

  copyButton.addEventListener("click", async function () {
    if (!generatedCode) return;
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(generatedCode);
      } else {
        codeOutput.focus();
        codeOutput.select();
        if (!document.execCommand("copy")) throw new Error("Clipboard access was blocked.");
      }
      setStatus("C++ header copied to the clipboard.", "good");
    } catch (error) {
      codeOutput.focus();
      codeOutput.select();
      setStatus("Clipboard access was blocked. The code is selected; use Ctrl+C.", "");
    }
  });

  exportButton.disabled = true;
  playButton.disabled = true;
  stopButton.disabled = true;
  allPresetScanButton.disabled = true;
  allPresetScanCopyButton.disabled = true;
  allPresetScanDownloadButton.disabled = true;
  downloadButton.disabled = true;
  copyButton.disabled = true;
  updateModeInfo();
  updateStopButton();
  detectKeyboardLayoutFromBrowser();
})();