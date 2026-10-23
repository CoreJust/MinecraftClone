(() => {
  "use strict";

  const stages = [
    { label: "Tectonics", caption: "Illustrative plate motion" },
    { label: "Relief", caption: "Raising ridges and islands" },
    { label: "Water", caption: "Filling coasts and basins" },
    { label: "Ice", caption: "Advancing glaciers and sea ice" },
    { label: "Climate", caption: "Circulation and rainfall" },
    { label: "Arrival", caption: "Locating the starting area" },
  ];
  const descriptions = [
    ["Continents drift.", "Rifts and collisions."],
    ["Mountains rise.", "Islands emerge."],
    ["Seas find the coast.", "Rivers reach basins."],
    ["Glaciers advance.", "Cold seas freeze."],
    ["Winds and currents.", "Rain finds wet ground."],
    ["A whole world.", "One starting point."],
  ];
  const baseLayers = {
    plates: false, heights: true, temperature: false, biomes: false,
    wind: false, currents: false, moisture: false, precipitation: false,
  };
  const defaultLayers = { ...baseLayers, biomes: true };
  const work = document.createElement("canvas");
  const workContext = work.getContext("2d");
  let cache = null;
  const clamp = (value, low = 0, high = 1) => Math.max(low, Math.min(high, value));
  const ease = (value) => value * value * (3 - 2 * value);
  const wrap = (value, size = 1) => ((value % size) + size) % size;
  const hasField = (atlas, name) => Boolean(atlas && atlas[name]
    && atlas[name].length === atlas.width * atlas.height);

  function nearestSources(water, width, height, wet) {
    const nearest = new Int32Array(water.length).fill(-1);
    const queue = new Int32Array(water.length);
    let end = 0;
    for (let index = 0; index < water.length; index += 1) {
      if (Boolean(water[index]) !== wet) continue;
      nearest[index] = index;
      queue[end++] = index;
    }
    for (let head = 0; head < end; head += 1) {
      const index = queue[head];
      const x = index % width;
      const y = Math.floor(index / width);
      for (const next of [
        y * width + wrap(x - 1, width), y * width + wrap(x + 1, width),
        wrap(y - 1, height) * width + x, wrap(y + 1, height) * width + x,
      ]) {
        if (nearest[next] !== -1) continue;
        nearest[next] = nearest[index];
        queue[end++] = next;
      }
    }
    return end ? nearest : null;
  }

  function prepare(atlas, normal, getRaster, iceMask) {
    if (cache && cache.atlas === atlas && cache.normal === normal && cache.iceMask === iceMask) return cache;
    const { width, height } = atlas;
    const count = width * height;
    if (count > 256 * 256 || normal.width !== width || normal.height !== height
      || !iceMask || iceMask.length !== count) return null;
    const geology = getRaster(atlas, baseLayers, { ice: false });
    const preIce = getRaster(atlas, defaultLayers, { ice: false });
    if (!geology || !preIce) return null;
    let pixels, rockPixels, preIcePixels;
    try {
      pixels = normal.getContext("2d").getImageData(0, 0, width, height).data;
      rockPixels = geology.getContext("2d").getImageData(0, 0, width, height).data;
      preIcePixels = preIce.getContext("2d").getImageData(0, 0, width, height).data;
    } catch (error) {
      return null;
    }
    const water = new Uint8Array(count);
    const strain = new Float32Array(count);
    const plateIndex = new Int16Array(count);
    const plateGroups = new Map();
    const motion = ["plateId", "plateVx", "plateVy"].every((name) => hasField(atlas, name));
    const temperature = hasField(atlas, "temperature");
    const moisture = hasField(atlas, "moisture");
    const precipitation = hasField(atlas, "precipitation");
    let lowest = Infinity, highest = -Infinity, moistureMax = 0, rainMax = 0;
    let iceMin = Infinity, iceMax = -Infinity;
    for (let index = 0; index < count; index += 1) {
      lowest = Math.min(lowest, atlas.elevation[index]);
      highest = Math.max(highest, atlas.elevation[index]);
      if (moisture) moistureMax = Math.max(moistureMax, atlas.moisture[index]);
      if (precipitation) rainMax = Math.max(rainMax, atlas.precipitation[index]);
      if (iceMask[index] && temperature) {
        iceMin = Math.min(iceMin, atlas.temperature[index]);
        iceMax = Math.max(iceMax, atlas.temperature[index]);
      }
      water[index] = (hasField(atlas, "oceanMask") && atlas.oceanMask[index] ? 1 : 0)
        | (hasField(atlas, "lakeMask") && atlas.lakeMask[index] ? 2 : 0)
        | (hasField(atlas, "riverMask") && atlas.riverMask[index] ? 4 : 0);
      if (!motion) continue;
      const id = atlas.plateId[index];
      if (!plateGroups.has(id)) plateGroups.set(id, { id, u: 0, v: 0, count: 0, index: plateGroups.size });
      const plate = plateGroups.get(id);
      plateIndex[index] = plate.index;
      plate.u += atlas.plateVx[index];
      plate.v += atlas.plateVy[index];
      plate.count += 1;
      const x = index % width;
      const y = Math.floor(index / width);
      for (const [next, nx, ny] of [
        [y * width + wrap(x + 1, width), 1, 0],
        [y * width + wrap(x - 1, width), -1, 0],
        [wrap(y + 1, height) * width + x, 0, 1],
        [wrap(y - 1, height) * width + x, 0, -1],
      ]) {
        if (atlas.plateId[next] === id) continue;
        strain[index] += (atlas.plateVx[index] - atlas.plateVx[next]) * nx
          + (atlas.plateVy[index] - atlas.plateVy[next]) * ny;
      }
    }
    const plates = [...plateGroups.values()];
    let maximumSpeed = 0, maximumStrain = 0;
    plates.forEach((plate) => {
      plate.u /= plate.count;
      plate.v /= plate.count;
      maximumSpeed = Math.max(maximumSpeed, Math.hypot(plate.u, plate.v));
    });
    for (const value of strain) maximumStrain = Math.max(maximumStrain, Math.abs(value));
    plates.forEach((plate) => {
      plate.u /= maximumSpeed || 1;
      plate.v /= maximumSpeed || 1;
    });
    for (let pass = 0; pass < 3; pass += 1) {
      const smoothed = new Float32Array(count);
      for (let index = 0; index < count; index += 1) {
        const x = index % width;
        const y = Math.floor(index / width);
        smoothed[index] = (strain[index] * 2
          + strain[y * width + wrap(x - 1, width)] + strain[y * width + wrap(x + 1, width)]
          + strain[wrap(y - 1, height) * width + x] + strain[wrap(y + 1, height) * width + x]) / 6;
      }
      strain.set(smoothed);
    }
    for (let index = 0; index < count; index += 1) strain[index] /= maximumStrain || 1;
    const landSources = nearestSources(water, width, height, false);
    const waterSources = nearestSources(water, width, height, true);
    if (!landSources || !waterSources || !Number.isFinite(lowest + highest)) return null;
    const scale = width <= 128 && height <= 128 ? 3 : 1;
    work.width = width * scale;
    work.height = height * scale;
    const renderCount = work.width * work.height;
    const wetness = new Float32Array(count);
    for (let index = 0; index < count; index += 1) {
      wetness[index] = (moisture ? atlas.moisture[index] / (moistureMax || 1) * 0.4 : 0)
        + (precipitation ? Math.log1p(atlas.precipitation[index]) / (Math.log1p(rainMax) || 1) * 0.6 : 0);
    }
    cache = {
      atlas, normal, width, height, count, pixels, rockPixels, preIcePixels, water, strain,
      plates, plateIndex, motion: motion && maximumSpeed > 0 && plates.length <= 64,
      temperature, moisture, moistureMax: moistureMax || 1, wetness,
      iceMask, iceMin, iceRange: Math.max(0.001, iceMax - iceMin),
      landSources, waterSources, range: Math.max(0.001, highest - lowest),
      scale, renderWidth: work.width, renderHeight: work.height, renderCount,
      sources: new Int32Array(renderCount), heights: new Float32Array(renderCount),
      image: workContext.createImageData(work.width, work.height), frameKey: "",
    };
    cache.wind = makePaths(cache, "windU", "windV", false, 16);
    cache.currents = makePaths(cache, "currentU", "currentV", true, 18);
    cache.rain = [];
    for (let y = 4; y < height; y += 8) for (let x = 4; x < width; x += 8) {
      const index = y * width + x;
      if (wetness[index] < 0.22 || iceMask[index] || (temperature && atlas.temperature[index] <= 0)) continue;
      cache.rain.push({ x, y, index, phase: ((x * 37 + y * 19) % 97) / 97 });
    }
    cache.rain.sort((a, b) => wetness[b.index] - wetness[a.index]);
    cache.rain.length = Math.min(32, cache.rain.length);
    return cache;
  }

  function makePaths(data, uName, vName, waterOnly, limit) {
    if (!hasField(data.atlas, uName) || !hasField(data.atlas, vName)) return [];
    const paths = [];
    const columns = waterOnly ? 8 : 4;
    for (let row = 0; row < columns; row += 1) for (let column = 0; column < columns; column += 1) {
      let x = (column + 0.5) * data.width / columns;
      let y = (row + 0.5) * data.height / columns;
      const points = [];
      for (let step = 0; step < 32; step += 1) {
        const index = Math.floor(y) * data.width + Math.floor(x);
        if (data.iceMask[index] || (waterOnly && !(data.water[index] & 3))) break;
        const u = data.atlas[uName][index], v = data.atlas[vName][index];
        const magnitude = Math.hypot(u, v);
        if (!Number.isFinite(magnitude) || magnitude < 1e-8) break;
        points.push({ x, y });
        const nextX = x + u / magnitude * 0.8;
        const nextY = y + v / magnitude * 0.8;
        if (nextX < 0 || nextX >= data.width || nextY < 0 || nextY >= data.height) break;
        x = nextX;
        y = nextY;
      }
      if (points.length > 6) paths.push({ points, phase: ((column * 23 + row * 41) % 31) });
    }
    paths.sort((a, b) => a.phase - b.phase);
    return paths.slice(0, limit);
  }

  function weather(data, progress) {
    const opacity = clamp(progress * 5);
    if (!opacity) return;
    const output = data.image.data;
    function plot(x, y, color, alpha, kind) {
      if (x < 0 || x >= data.width || y < 0 || y >= data.height) return;
      const left = Math.min(data.renderWidth - 1, Math.round(x * data.scale));
      const top = Math.min(data.renderHeight - 1, Math.round(y * data.scale));
      const source = Math.floor(top / data.scale) * data.width + Math.floor(left / data.scale);
      if (data.iceMask[source] || (kind === "current" && !(data.water[source] & 3))
        || (kind === "rain" && data.wetness[source] < 0.22)) return;
      const pixel = (top * data.renderWidth + left) * 4;
      for (let channel = 0; channel < 3; channel += 1) {
        output[pixel + channel] += (color[channel] - output[pixel + channel]) * alpha * opacity;
      }
    }
    function line(from, to, color, alpha, kind) {
      const steps = Math.max(1, Math.ceil(Math.max(Math.abs(to.x - from.x), Math.abs(to.y - from.y)) * data.scale));
      for (let step = 0; step <= steps; step += 1) {
        const p = step / steps;
        plot(from.x + (to.x - from.x) * p, from.y + (to.y - from.y) * p, color, alpha, kind);
      }
    }
    for (const [paths, color, kind] of [
      [data.wind, [99, 219, 212], "wind"], [data.currents, [220, 238, 247], "current"],
    ]) for (const path of paths) {
      const head = wrap(progress * 26 + path.phase, path.points.length - 1);
      for (let tail = 5; tail >= 0; tail -= 1) {
        const position = head - tail;
        if (position < 0) continue;
        const index = Math.floor(position);
        const next = Math.min(path.points.length - 1, index + 1);
        const fraction = tail ? 1 : position - index;
        const from = path.points[index];
        const to = {
          x: from.x + (path.points[next].x - from.x) * fraction,
          y: from.y + (path.points[next].y - from.y) * fraction,
        };
        line(from, to, color, 0.2 + (5 - tail) * 0.1, kind);
      }
    }
    for (const drop of data.rain) {
      const p = wrap(progress * 4 + drop.phase);
      const u = hasField(data.atlas, "windU") ? data.atlas.windU[drop.index] : 0;
      const v = hasField(data.atlas, "windV") ? data.atlas.windV[drop.index] : 0;
      const magnitude = Math.hypot(u, v) || 1;
      const x = drop.x + u / magnitude * p * 1.5;
      const y = drop.y + p * 2.5;
      line({ x, y }, { x: x + u / magnitude * 0.35, y: y + 0.65 }, [176, 211, 228], 0.68, "rain");
    }
  }

  function frame(data, stageIndex, progress) {
    const tick = Math.round(progress * 120);
    if (stageIndex >= 5) return data.normal;
    progress = tick / 120;
    const frameKey = stageIndex + ":" + tick;
    if (data.frameKey === frameKey) return work;
    data.frameKey = frameKey;
    const p = ease(progress);
    const drift = data.motion ? stageIndex === 0 ? 3 - p * 1.8 : stageIndex === 1 ? 1.2 * (1 - p) : 0 : 0;
    const uplift = data.motion ? stageIndex === 0 ? p * 0.8 : stageIndex === 1 ? (1 - p) * 0.8 : 0 : 0;
    const heightScale = stageIndex === 0 ? 0.85 + p * 0.08 : stageIndex === 1 ? 0.93 + p * 0.07 : 1;
    const seaLevel = stageIndex === 0 ? -0.3 + p * 0.1
      : stageIndex === 1 ? -0.2 - p * 0.45 : stageIndex === 2 ? -0.65 * (1 - p) : 0;
    const drainage = stageIndex < 2 ? 0 : stageIndex === 2 ? p : 1;
    const ice = stageIndex < 3 ? 0 : stageIndex === 3 ? p : 1;
    const climate = stageIndex === 4 ? p : 0;
    const { atlas, width, height, count, scale, renderWidth, renderHeight, renderCount, sources, heights } = data;

    if (drift) {
      sources.fill(-1);
      heights.fill(-Infinity);
      // Every native cell in a plate receives the same translation; patches never stretch.
      for (let source = 0; source < count; source += 1) {
        const plate = data.plates[data.plateIndex[source]];
        const x = Math.round((source % width - plate.u * drift) * scale);
        const y = Math.round((Math.floor(source / width) - plate.v * drift) * scale);
        const elevation = atlas.elevation[source] * heightScale + data.strain[source] * uplift;
        for (let oy = 0; oy < scale; oy += 1) for (let ox = 0; ox < scale; ox += 1) {
          const target = wrap(y + oy, renderHeight) * renderWidth + wrap(x + ox, renderWidth);
          if (elevation <= heights[target]) continue;
          sources[target] = source;
          heights[target] = elevation;
        }
      }
    }
    for (let index = 0; index < renderCount; index += 1) {
      const native = Math.floor(Math.floor(index / renderWidth) / scale) * width
        + Math.floor((index % renderWidth) / scale);
      if (!drift) {
        sources[index] = native;
        heights[index] = atlas.elevation[native] * heightScale + data.strain[native] * uplift;
      } else if (sources[index] < 0) {
        sources[index] = data.waterSources[native];
        heights[index] = Math.min(atlas.elevation[sources[index]], seaLevel - 0.2);
      }
    }

    const output = data.image.data;
    for (let index = 0; index < renderCount; index += 1) {
      const source = sources[index];
      const level = heights[index];
      const mask = data.water[source];
      const targetWater = hasField(atlas, "waterLevel") ? atlas.waterLevel[source] : 0;
      const riverHeight = clamp(targetWater / data.range);
      const wet = level <= seaLevel || ((mask & 2) && drainage > 0
        && level <= seaLevel + (targetWater - seaLevel) * drainage)
        || ((mask & 4) && drainage >= riverHeight && drainage > 0);
      const colorSource = wet ? mask ? source : data.waterSources[source]
        : mask ? data.landSources[source] : source;
      const pixel = colorSource * 4;
      const x = index % renderWidth;
      const y = Math.floor(index / renderWidth);
      const native = Math.floor(y / scale) * width + Math.floor(x / scale);
      const west = y * renderWidth + wrap(x - scale, renderWidth);
      const east = y * renderWidth + wrap(x + scale, renderWidth);
      const north = wrap(y - scale, renderHeight) * renderWidth + x;
      const south = wrap(y + scale, renderHeight) * renderWidth + x;
      const sx = source % width;
      const sy = Math.floor(source / width);
      const originalSlope = atlas.elevation[sy * width + wrap(sx - 1, width)]
        - atlas.elevation[sy * width + wrap(sx + 1, width)]
        + (atlas.elevation[wrap(sy - 1, height) * width + sx]
          - atlas.elevation[wrap(sy + 1, height) * width + sx]) * 0.65;
      const slope = heights[west] - heights[east] + (heights[north] - heights[south]) * 0.65;
      const shade = wet ? 1 : clamp(1 + (slope - originalSlope) * 4 / data.range, 0.65, 1.25);
      const moisture = data.moisture ? atlas.moisture[source] / data.moistureMax : 0.5;
      const climateThreshold = 0.15 + (1 - moisture) * 0.65;
      const cover = climate ? clamp((climate - climateThreshold + 0.2) / 0.2) : 0;
      const rank = data.temperature ? clamp((atlas.temperature[native] - data.iceMin) / data.iceRange) : 0;
      const frozen = data.iceMask[native] && ice ? clamp((ice * 1.12 - rank) / 0.12) : 0;
      for (let channel = 0; channel < 3; channel += 1) {
        const bare = wet ? data.preIcePixels[pixel + channel] : data.rockPixels[pixel + channel];
        let value = bare * shade;
        value += (data.preIcePixels[pixel + channel] - value) * cover;
        if (frozen) value += (data.pixels[native * 4 + channel] - value) * frozen;
        output[index * 4 + channel] = value;
      }
      output[index * 4 + 3] = 255;
    }
    if (stageIndex === 4) weather(data, progress);
    workContext.putImageData(data.image, 0, 0);
    return work;
  }

  function drawMarker(api, bounds, player) {
    if (!player || !Number.isFinite(player.x) || !Number.isFinite(player.y)) return;
    const x = bounds.x + wrap(player.x) * bounds.width;
    const y = bounds.y + wrap(player.y) * bounds.height;
    api.outlinedText("+", x - 8, y - 16, 2, "#ffe49a", "#18303b");
    api.outlinedText("YOU", x + 12, y + 8, 1, "#ffe49a", "#18303b");
  }

  function drawCardinals(api, bounds) {
    const scale = api.height < 500 || api.width < 1080 ? 1 : 2;
    const { x, y, width, height } = bounds;
    for (const [label, left, top] of [
      ["N", x + width / 2 - 4 * scale, y + 6],
      ["S", x + width / 2 - 4 * scale, y + height - 16 * scale - 6],
      ["W", x + 7, y + height / 2 - 8 * scale],
      ["E", x + width - 8 * scale - 7, y + height / 2 - 8 * scale],
    ]) api.outlinedText(label, left, top, scale, "#e6ead7", "#233b43");
  }

  function draw(api, model) {
    const { context, width, height, colors } = api;
    const margin = width < 640 ? 16 : 28;
    const wide = width >= 760 && height >= 320;
    const titleScale = width >= 360 ? 2 : 1;
    const bodyScale = wide ? 2 : 1;
    const phase = model.phase || "sample";
    const stageIndex = phase === "ready" ? stages.length - 1
      : clamp(Math.floor(model.stageIndex || 0), 0, stages.length - 1);
    const progress = phase === "ready" || model.reducedMotion ? 1
      : clamp(Number.isFinite(model.stageProgress) ? model.stageProgress : 0);
    const bodyTop = margin + titleScale * 16 + (wide ? height < 500 ? 48 : 72 : 56);
    const sideWidth = width >= 1100 ? 300 : 260;
    const fieldX = wide ? margin + sideWidth + 28 : margin;
    const fieldWidth = width - fieldX - margin;
    const fieldY = wide ? margin : bodyTop + 116;
    const fieldBottom = height - margin - (wide ? 0 : 78);
    const atlas = api.atlas || window.S2_WORLD_DATA;
    const renderer = window.S2WorldMapView;
    const dataReady = atlas && Number.isInteger(atlas.width) && Number.isInteger(atlas.height)
      && atlas.width > 0 && atlas.height > 0 && hasField(atlas, "elevation");
    const getRaster = dataReady && renderer && typeof renderer.getRaster === "function"
      ? renderer.getRaster : null;
    const normal = getRaster ? getRaster(atlas, defaultLayers) : null;
    const iceMask = renderer && typeof renderer.getIceMask === "function" ? renderer.getIceMask(atlas) : null;
    const data = normal && stageIndex < stages.length - 1 ? prepare(atlas, normal, getRaster, iceMask) : null;
    const terrain = data ? frame(data, stageIndex, progress) : normal;
    const mapReady = Boolean(terrain && terrain.width > 0 && terrain.height > 0);

    context.fillStyle = "#15272f";
    context.fillRect(0, 0, width, height);
    api.text("Creating your world", margin, margin, titleScale, colors.light);
    if (dataReady) api.text("Seed " + (atlas.seed === undefined ? "?" : atlas.seed) + " / Illustrative history",
      margin, margin + titleScale * 16 + 12, 1, colors.quiet);
    api.text(String(stageIndex + 1).padStart(2, "0") + " / " + String(stages.length).padStart(2, "0"),
      margin, bodyTop, 1, colors.quiet);
    api.text(stages[stageIndex].label, margin, bodyTop + 24, titleScale, colors.accent);
    const descriptionY = bodyTop + 28 + titleScale * 16;
    descriptions[stageIndex].forEach((line, index) => {
      api.text(line, margin, descriptionY + index * (bodyScale * 16 + 6), bodyScale, colors.light);
    });

    if (wide && height >= 560) {
      const listScale = 1;
      const listY = Math.max(descriptionY + bodyScale * 38 + 28,
        height - margin - 110 - stages.length * (listScale * 16 + 12));
      stages.forEach((stage, index) => {
        const prefix = index < stageIndex ? "+ " : index === stageIndex ? "> " : "  ";
        api.text(prefix + stage.label, margin, listY + index * (listScale * 16 + 12), listScale,
          index === stageIndex ? colors.accent : index < stageIndex ? colors.light : colors.quiet);
      });
    }

    if (mapReady) {
      const scale = Math.min(fieldWidth / terrain.width, Math.max(1, fieldBottom - fieldY) / terrain.height);
      const bounds = {
        width: Math.floor(terrain.width * scale), height: Math.floor(terrain.height * scale),
      };
      bounds.x = Math.round(fieldX + (fieldWidth - bounds.width) / 2);
      bounds.y = Math.round(fieldY + (fieldBottom - fieldY - bounds.height) / 2);
      context.save();
      context.imageSmoothingEnabled = false;
      context.globalAlpha = 1;
      context.drawImage(terrain, bounds.x, bounds.y, bounds.width, bounds.height);
      context.restore();
      if (stageIndex === stages.length - 1) drawMarker(api, bounds, model.player);
      drawCardinals(api, bounds);
    } else {
      api.text("World data unavailable", fieldX, fieldY + 48, 1, colors.light);
      api.text("The world preview cannot be drawn.", fieldX, fieldY + 72, 1, colors.quiet);
    }

    let status = !mapReady ? "World data unavailable"
      : model.reducedMotion ? "Reduced motion - Enter advances" : stages[stageIndex].caption;
    let action = "Enter  Next stage     Esc  Cancel";
    let statusColor = colors.quiet;
    if (phase === "ready") {
      status = "World ready";
      action = "Press any key to start";
      statusColor = colors.light;
    } else if (phase === "cancelled") {
      status = "Generation cancelled";
      action = "Enter  Retry";
    } else if (phase === "error") {
      status = "World preview unavailable";
      action = "Enter  Retry     Esc  Cancel";
      statusColor = colors.accent;
    } else if (mapReady && stageIndex < stages.length - 1 && !data) {
      status = "Time-lapse data unavailable";
    } else if (data && stageIndex === 0 && !data.motion) {
      status = "Plate motion unavailable";
    } else if (data && stageIndex === 3 && !data.iceMask) {
      status = "Ice data unavailable";
    }
    const footerScale = phase === "ready" && width >= 760 ? 2 : 1;
    api.text(status, margin, height - margin - footerScale * 32 - 8, footerScale, statusColor);
    api.text(action, margin, height - margin - footerScale * 16, footerScale, colors.accent);
    if (phase === "ready") api.text("Esc  Cancel", width - margin - 88,
      height - margin - 16, 1, colors.quiet);
  }

  window.S2GenerationView = { stages, durationMs: 30000, draw };
})();
