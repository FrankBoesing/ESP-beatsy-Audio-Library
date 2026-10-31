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
  var outputInfo = document.getElementById("sf2OutputInfo");
  var wavetableModeInput = document.getElementById("sf2WavetableMode");
  var keyboardStartSelect = document.getElementById("sf2KeyboardStart");
  var previewVolumeInput = document.getElementById("sf2PreviewVolume");
  var keyboardElement = document.getElementById("sf2Keyboard");
  var modeInfo = document.getElementById("sf2ModeInfo");

  if (!fileInput || !presetSelect || !velocityInput || !exportButton) return;

  var font = null;
  var currentFileName = "";
  var loadGeneration = 0;
  var audioContext = null;
  var masterGain = null;
  var activeVoices = [];
  var keyboardHolds = Object.create(null);
  var pointerHolds = Object.create(null);
  var pcHolds = Object.create(null);
  var pcNoteByCode = Object.create(null);
  var legacyStartToken = 0;
  var generatedCode = "";
  var generatedFileName = "";
  
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

  var INDEX_BITS = 16;
  var MAX_SAMPLE_FRAMES = 65535;
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
            if (SUPPORTED_OPERATORS.indexOf(op) < 0 && values[key] !== 0) {
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

      for (var wi = 0; wi < preset.zones.length; wi++) {
        if (preset.zones[wi].unsupportedGenerators.length) {
          preset.warnings.push("Unsupported generator(s): " +
            preset.zones[wi].unsupportedGenerators.join(", ") + ".");
        }
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

  function updatePresetInfo() {
    presetInfo.textContent = "";
    if (!font) return;
    var preset = font.presets[parseInt(presetSelect.value, 10)];
    if (!preset) return;

    var velocity = Math.max(1, Math.min(127, parseInt(velocityInput.value, 10) || 100));
    var supported = 0;
    var errors = [];
    var overlappingNotes = 0;

    for (var note = 0; note < 128; note++) {
      var matches = regionsForNote(preset, note, velocity);
      if (matches.length > 1) overlappingNotes++;
      if (matches.length === 1 && !makeRegionLayout(font, matches[0]).error) supported++;
    }

    var lines = [
      "Preset: " + preset.name,
      "Bank / program: " + preset.bank + " / " + preset.program,
      "Instrument zones: " + preset.zones.length,
      "Notes covered at velocity " + velocity + ": " + supported + " / 128",
      "Notes with overlapping layers at this velocity: " + overlappingNotes
    ];
    if (font.modulatorCount) {
      lines.push("Modulators present: " + font.modulatorCount + " (not rendered in V1).");
    }
    if (font.sample24Chunk) lines.push("24-bit sample data: sm24 found; rounded to 16-bit for this engine.");
    for (var i = 0; i < preset.warnings.length; i++) lines.push("Warning: " + preset.warnings[i]);
    if (overlappingNotes) {
      lines.push("Layering can be auditioned in the browser, but this legacy instrument format cannot export overlapping layers.");
    }
    presetInfo.textContent = lines.join("\n");
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

  var KEYBOARD_PC_CODES = [
    "KeyY", "KeyS", "KeyX", "KeyD", "KeyC", "KeyV",
    "KeyG", "KeyB", "KeyH", "KeyN", "KeyJ", "KeyM",
    "KeyQ", "Digit2", "KeyW", "Digit3", "KeyE", "KeyR",
    "Digit5", "KeyT", "Digit6", "KeyZ", "Digit7", "KeyU"
  ];
  var KEYBOARD_PC_LABELS = [
    "Z", "S", "X", "D", "C", "V", "G", "B", "H", "N", "J", "M",
    "Q", "2", "W", "3", "E", "R", "5", "T", "6", "Y", "7", "U"
  ];
  var BLACK_KEY_OFFSETS = [1, 3, 6, 8, 10];
  var MAX_PREVIEW_VOICES = 16;
  var MAX_PREVIEW_LAYERS_PER_NOTE = 8;
  var MAX_PREVIEW_SAMPLE_FRAMES = 4000000;
  var MAX_PREVIEW_CACHE_FRAMES = 10000000;
  var WAVETABLE_DEFAULT_AMPLITUDE = 90 / 127;

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
        "Wavetable-kompatibel: monophon, genau eine Sample-Zone pro Note, feste Velocity, Mono-Downmix, Standard-Amplitude 90/127 und maximal 65.535 Sampleframes. Note-Off startet Release; Loop-until-release bleibt wie im aktuellen Synthesizer auch während Release aktiv. SF2-Filter, Modulatoren, Pan und Effektsends werden nicht gerendert.";
    } else {
      modeInfo.textContent =
        "Browser-Vorschau: polyphon (bis zu " + MAX_PREVIEW_VOICES +
        " gleichzeitig gehaltene Noten) und mit mehreren übereinanderliegenden Sample-Zonen. Die Velocity bleibt der eingestellte Wert. Nicht implementierte SF2-Filter, Modulatoren, Pan und Effektsends werden weiterhin nicht gerendert.";
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
      pcWhite.textContent = KEYBOARD_PC_LABELS[w];
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
      blackLabel.textContent = KEYBOARD_PC_LABELS[b];
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
      masterGain.gain.value = previewVolumeInput ?
        Number(previewVolumeInput.value) / 100 : 0.7;
      masterGain.connect(audioContext.destination);
    }
    if (audioContext.state === "suspended") {
      return audioContext.resume().then(function () { return audioContext; });
    }
    return Promise.resolve(audioContext);
  }

  function updatePreviewVolume() {
    if (masterGain && audioContext) {
      var value = previewVolumeInput ? Number(previewVolumeInput.value) / 100 : 0.7;
      masterGain.gain.setTargetAtTime(value, audioContext.currentTime, 0.015);
    }
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
    if (soundFont.audioBufferFrameCount + layout.frames > MAX_PREVIEW_CACHE_FRAMES) {
      throw new Error("The preview sample cache reached its " +
        MAX_PREVIEW_CACHE_FRAMES.toLocaleString() +
        "-frame memory limit. Load the SoundFont again to clear the preview cache.");
    }
    var pcm = getPcm(soundFont, region);
    var buffer = audioContext.createBuffer(1, pcm.length - 1, layout.sampleRate);
    var output = buffer.getChannelData(0);
    for (var i = 0; i < output.length; i++) output[i] = pcm[i] / 32768;
    soundFont.audioBufferCache[cacheKey] = buffer;
    soundFont.audioBufferFrameCount += layout.frames;
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
    legacyStartToken++;
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

      // SF2 mode 3 is loop-until-release. The current wavetable engine does
      // not leave such loops during release; the general browser mode does.
      if (!voice.wavetableMode && layer.layout.loopMode === 3) {
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
    var preset = currentPreset();
    if (!preset) return;

    note = Math.max(0, Math.min(127, Math.round(note)));
    velocity = Math.max(1, Math.min(127, Math.round(velocity)));
    var wavetableMode = isWavetableMode();
    var matches = regionsForNote(preset, note, velocity);

    if (!matches.length) {
      setStatus("No SF2 zone matches " + midiName(note) +
        " at velocity " + velocity + ".", "");
      return;
    }

    if (matches.length > MAX_PREVIEW_LAYERS_PER_NOTE) {
      setStatus("Preview refused: " + matches.length +
        " zones match this note; the browser preview limit is " +
        MAX_PREVIEW_LAYERS_PER_NOTE + " layers per note.", "bad");
      return;
    }

    if (wavetableMode) {
      if (matches.length !== 1) {
        setStatus("Wavetable-compatible preview refused: " + midiName(note) +
          " has " + matches.length + " overlapping zones at velocity " +
          velocity + ". The current AudioSynthWavetable API supports one sample per note.", "bad");
        return;
      }
      var compatibleLayout = makeRegionLayout(font, matches[0]);
      if (compatibleLayout.error) {
        setStatus("Preview failed: " + compatibleLayout.error, "bad");
        return;
      }
      if (compatibleLayout.frames > MAX_SAMPLE_FRAMES) {
        setStatus("Wavetable-compatible preview refused: this sample has " +
          compatibleLayout.frames.toLocaleString() +
          " frames; the current 16-bit sample index supports at most " +
          MAX_SAMPLE_FRAMES + ".", "bad");
        return;
      }
    }

    var requestToken = wavetableMode ? ++legacyStartToken : 0;
    if (wavetableMode) {
      hardStopAllVoices();
    } else {
      var unReleasedVoices = activeVoices.filter(function (voice) { return !voice.released; });
      if (unReleasedVoices.length >= MAX_PREVIEW_VOICES && unReleasedVoices.length) {
        releaseVoice(unReleasedVoices[0]);
      }
    }

    try {
      await ensureAudioContext();
      if (wavetableMode && requestToken !== legacyStartToken) return;
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
          source.buffer = getAudioBuffer(font, region);
          source.playbackRate.value = playbackRatio(font, region, note);
          source.loop = layout.loop;
          if (source.loop) {
            source.loopStart = layout.loopStart / layout.sampleRate;
            source.loopEnd = layout.loopEnd / layout.sampleRate;
          }

          var gainScale = wavetableMode ? WAVETABLE_DEFAULT_AMPLITUDE : 1;
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
        (wavetableMode ? " Wavetable-compatible mode." : " Browser polyphonic mode."), "good");
    } catch (error) {
      setStatus("Preview failed: " + error.message, "bad");
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
    return !!target.closest("input, select, textarea, [contenteditable='true']");
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
      (Math.pow(2, INDEX_BITS) * midiFrequency(note));
    var env = envelopeForNote(values, note);
    var attenuation = Math.max(0, Math.min(1440, values[GEN.attenuation]));
    var initialScalar = Math.round(65535 * Math.pow(10, -attenuation / 200));
    var sustainGain = Math.pow(10, -env.sustainCb / 200);
    var sustainMultiplier = Math.round(2147483647 * (1 - sustainGain));
    var maxPhase = layout.frames * Math.pow(2, 32 - INDEX_BITS);
    var loopEnd = layout.loop ? layout.loopEnd * Math.pow(2, 32 - INDEX_BITS) : 0;
    var loopLength = layout.loop ?
      (layout.loopEnd - layout.loopStart) * Math.pow(2, 32 - INDEX_BITS) : 0;
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
        } else if (layout.frames > MAX_SAMPLE_FRAMES) {
          noteErrors.push(midiName(note) + " uses " + layout.frames +
            " sample frames; the current wavetable index is limited to " + MAX_SAMPLE_FRAMES);
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

    stopPreview();
    resetOutput();
    font = null;
    currentFileName = "";
    presetSelect.innerHTML = "";
    presetSelect.disabled = true;
    presetInfo.textContent = "";
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
      setStatus("The selected file exceeds the V1 safety limit of 512 MiB.", "bad");
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
    } catch (error) {
      if (thisLoad !== loadGeneration) return;
      font = null;
      currentFileName = "";
      presetSelect.innerHTML = "";
      presetSelect.disabled = true;
      metadata.innerHTML = "";
      summary.textContent = "";
      renderKeyboard();
      exportButton.disabled = true;
      playButton.disabled = true;
      setStatus("Could not load SoundFont: " + error.message, "bad");
    }
  });

  presetSelect.addEventListener("change", function () {
    stopPreview();
    resetOutput();
    updatePresetInfo();
  });
  velocityInput.addEventListener("change", function () {
    stopPreview();
    resetOutput();
    updatePresetInfo();
  });
  playButton.addEventListener("click", playPreview);
  stopButton.addEventListener("click", stopPreview);

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

  window.addEventListener("keydown", function (event) {
    if (event.repeat || isTypingTarget(event.target)) return;
    var note = pcNoteByCode[event.code];
    if (note === undefined) return;
    event.preventDefault();
    var sourceId = "pc:" + event.code;
    if (pcHolds[event.code]) return;
    pcHolds[event.code] = true;
    holdKeyboardNote(note, sourceId);
  });

  window.addEventListener("keyup", function (event) {
    if (!pcHolds[event.code]) return;
    var note = pcNoteByCode[event.code];
    delete pcHolds[event.code];
    if (note !== undefined) releaseKeyboardNote(note, "pc:" + event.code);
  });

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
      updateModeInfo();
    });
  }
  if (previewVolumeInput) {
    previewVolumeInput.addEventListener("input", updatePreviewVolume);
  }

  exportButton.addEventListener("click", async function () {
    if (!font) return;
    stopPreview();
    exportButton.disabled = true;
    resetOutput();
    setStatus("Resolving zones and generating the instrument header…", "");
    try {
      var result = await generateHeader();
      generatedCode = result.code;
      generatedFileName = result.fileName;
      codeOutput.value = generatedCode;
      codePanel.classList.remove("hidden");
      downloadButton.disabled = false;
      copyButton.disabled = false;
      outputInfo.textContent = "Generated " + result.fileName + " · " +
        result.mappedNotes + " mapped notes · " + result.sampleCount +
        " unique sample arrays · " + result.uniqueFrames.toLocaleString() +
        " PCM frames · velocity " + result.velocity + ".";
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
  downloadButton.disabled = true;
  copyButton.disabled = true;
  updateModeInfo();
  updateStopButton();
})();