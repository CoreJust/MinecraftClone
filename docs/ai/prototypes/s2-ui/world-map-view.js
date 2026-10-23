(() => {
  "use strict";

  const PLAYER = { x: 148 / 256, y: 112 / 256 };
  const CACHE_LIMIT = 6;
  const caches = new WeakMap();
  const layers = [
    { id: "plates", key: "P", name: "PLATES", color: "#d5a4ed", fields: ["plateId"] },
    { id: "heights", key: "H", name: "HEIGHTS", color: "#d9ceaa", fields: ["elevation"] },
    { id: "temperature", key: "T", name: "TEMPERATURE", color: "#efb77b", fields: ["temperature"] },
    { id: "biomes", key: "B", name: "BIOMES", color: "#a8c893", fields: ["climateZone"] },
    { id: "wind", key: "W", name: "WIND", color: "#63dbd4", fields: ["windU", "windV"] },
    { id: "currents", key: "C", name: "CURRENTS", color: "#dceef7", fields: ["currentU", "currentV"] },
    { id: "moisture", key: "O", name: "MOISTURE", color: "#79c2a8", fields: ["moisture"] },
    { id: "precipitation", key: "Q", name: "PRECIPITATION", color: "#c9a8e6", fields: ["precipitation"] },
  ];
  const climateColors = [
    [35, 69, 87], [181, 153, 99], [76, 125, 91],
    [119, 148, 107], [153, 172, 154], [222, 231, 224],
  ];
  const climateNames = ["Ocean", "Warm dry", "Warm wet", "Temperate", "Cold", "Snow"];

  function wrap(value) {
    return ((value % 1) + 1) % 1;
  }

  function clamp(value, minimum, maximum) {
    return Math.max(minimum, Math.min(maximum, value));
  }

  function createState() {
    return {
      zoom: 1, panX: PLAYER.x - 0.5, panY: PLAYER.y - 0.5,
      overview: false, legendExpanded: false,
      layers: {
        plates: false, heights: true, temperature: false, biomes: true,
        wind: false, currents: false, moisture: false, precipitation: false,
      },
    };
  }

  function handleKey(state, key, shiftKey) {
    const lowerKey = String(key).toLowerCase();
    const layer = layers.find((item) => item.key.toLowerCase() === lowerKey);
    if (layer) {
      state.layers[layer.id] = !state.layers[layer.id];
      return true;
    }
    if (lowerKey === "l") {
      state.legendExpanded = !state.legendExpanded;
      return true;
    }
    if (key === "0") {
      state.overview = true;
      state.zoom = 1;
      state.panX = 0;
      state.panY = 0;
      return true;
    }
    const step = (shiftKey ? 0.24 : 0.08) / state.zoom;
    if (key === "ArrowLeft") state.panX -= step;
    else if (key === "ArrowRight") state.panX += step;
    else if (key === "ArrowUp") state.panY -= step;
    else if (key === "ArrowDown") state.panY += step;
    else if (key === "+" || key === "=") state.zoom = Math.min(6, state.zoom + 0.25);
    else if (key === "-" || key === "_") state.zoom = Math.max(1, state.zoom - 0.25);
    else if (lowerKey === "r") {
      state.zoom = 1;
      state.panX = PLAYER.x - 0.5;
      state.panY = PLAYER.y - 0.5;
    } else return false;
    state.overview = false;
    state.panX = wrap(state.panX + 0.5) - 0.5;
    state.panY = wrap(state.panY + 0.5) - 0.5;
    return true;
  }

  function niceStep(value) {
    if (!(value > 0)) return 1;
    const power = 10 ** Math.floor(Math.log10(value));
    const fraction = value / power;
    return (fraction < 1.5 ? 1 : fraction < 3.5 ? 2 : fraction < 7.5 ? 5 : 10) * power;
  }

  function number(value, places = 2) {
    if (!Number.isFinite(value)) return "--";
    if (Math.abs(value) >= 10000) return value.toExponential(1);
    return String(Number(value.toFixed(places)));
  }

  function prepare(atlas) {
    const width = Math.floor(atlas && atlas.width);
    const height = Math.floor(atlas && atlas.height);
    if (!(width > 0 && height > 0 && width * height <= 512 * 512)) return null;
    const fields = {};
    const stats = {};
    const names = new Set([
      ...layers.flatMap((layer) => layer.fields), "oceanMask", "lakeMask", "riverMask",
      "waterLevel", "plateVx", "plateVy",
    ]);
    names.forEach((name) => {
      const values = atlas[name];
      if (!values || values.length !== width * height) return;
      let minimum = Infinity;
      let maximum = -Infinity;
      for (let index = 0; index < values.length; index += 1) {
        if (!Number.isFinite(values[index])) continue;
        minimum = Math.min(minimum, values[index]);
        maximum = Math.max(maximum, values[index]);
      }
      if (minimum === Infinity) return;
      fields[name] = values;
      stats[name] = { minimum, maximum, step: niceStep((maximum - minimum) / 8) };
    });
    const available = {};
    layers.forEach((layer) => {
      available[layer.id] = layer.fields.every((name) => Boolean(fields[name]));
    });
    const plateCenters = [];
    if (fields.plateId) {
      const plates = new Map();
      fields.plateId.forEach((id, index) => {
        if (!Number.isFinite(id) || id < 0) return;
        if (!plates.has(id)) plates.set(id, { id, x: 0, y: 0, count: 0, distance: Infinity });
        const plate = plates.get(id);
        plate.x += index % width;
        plate.y += Math.floor(index / width);
        plate.count += 1;
      });
      plates.forEach((plate) => {
        plate.x /= plate.count;
        plate.y /= plate.count;
      });
      fields.plateId.forEach((id, index) => {
        const plate = plates.get(id);
        if (!plate) return;
        const x = index % width;
        const y = Math.floor(index / width);
        const distance = (x - plate.x) ** 2 + (y - plate.y) ** 2;
        if (distance >= plate.distance) return;
        plate.distance = distance;
        plate.index = index;
      });
      plates.forEach((plate) => plateCenters.push({
        id: plate.id, index: plate.index,
        x: ((plate.index % width) + 0.5) / width,
        y: (Math.floor(plate.index / width) + 0.5) / height,
      }));
    }
    return { atlas, width, height, fields, stats, available, plateCenters, rasters: new Map() };
  }

  function isWater(data, index) {
    const f = data.fields;
    if (f.oceanMask || f.lakeMask) {
      return Boolean((f.oceanMask && f.oceanMask[index]) || (f.lakeMask && f.lakeMask[index]));
    }
    return Boolean(f.elevation && f.waterLevel && f.elevation[index] < f.waterLevel[index]);
  }

  function mix(color, tint, alpha) {
    for (let channel = 0; channel < 3; channel += 1) {
      color[channel] = color[channel] * (1 - alpha) + tint[channel] * alpha;
    }
  }

  function fraction(value, stats) {
    return clamp((value - stats.minimum) / (stats.maximum - stats.minimum || 1), 0, 1);
  }

  function ramp(value, stops) {
    if (value <= stops[0][0]) return [...stops[0][1]];
    for (let index = 1; index < stops.length; index += 1) {
      if (value > stops[index][0]) continue;
      const previous = stops[index - 1];
      const next = stops[index];
      const blend = (value - previous[0]) / (next[0] - previous[0]);
      return previous[1].map((channel, color) => channel + (next[1][color] - channel) * blend);
    }
    return [...stops[stops.length - 1][1]];
  }

  function temperatureColor(value) {
    return ramp(value, [
      [-40, [42, 78, 116]], [-20, [99, 153, 180]], [0, [202, 215, 204]],
      [15, [222, 187, 125]], [30, [205, 120, 83]],
    ]);
  }

  function heatColor(id, value, stats) {
    if (id === "temperature") return temperatureColor(value);
    let position = fraction(value, stats);
    if (id === "precipitation") {
      position = Math.log1p(Math.max(0, value - stats.minimum))
        / Math.log1p(stats.maximum - stats.minimum || 1);
    }
    return ramp(position, id === "moisture"
      ? [[0, [209, 189, 137]], [0.5, [109, 183, 158]], [1, [31, 137, 125]]]
      : [[0, [220, 213, 235]], [0.5, [163, 130, 199]], [1, [99, 73, 170]]]);
  }

  function activeHeat(data, selected) {
    return ["temperature", "moisture", "precipitation"]
      .filter((id) => data && data.available[id] && selected[id]);
  }

  function heatAlpha(count) {
    return count === 1 ? 0.44 : count === 2 ? 0.54 : count === 3 ? 0.6 : 0;
  }

  function elevationColor(value, stats) {
    if (value < 0) return ramp(value, [
      [Math.min(-0.01, stats.minimum), [24, 51, 70]], [0, [101, 150, 155]],
    ]);
    return ramp(value / Math.max(0.01, stats.maximum), [
      [0, [126, 151, 119]], [0.35, [177, 170, 123]],
      [0.7, [199, 184, 153]], [1, [237, 234, 217]],
    ]);
  }

  function contour(values, index, west, north, step) {
    if (!values || !Number.isFinite(values[index])) return false;
    const band = Math.floor(values[index] / step);
    return band !== Math.floor(values[west] / step) || band !== Math.floor(values[north] / step);
  }

  function raster(data, selected, options) {
    const ice = getIceMask(data.atlas);
    const showIce = options.ice !== false;
    const key = (showIce ? "ice:" : "bare:") + ["plates", "heights", "temperature", "biomes", "moisture", "precipitation"]
      .map((id) => selected[id] && data.available[id] ? "1" : "0").join("");
    if (data.rasters.has(key)) {
      const cached = data.rasters.get(key);
      data.rasters.delete(key);
      data.rasters.set(key, cached);
      return cached;
    }
    const canvas = document.createElement("canvas");
    canvas.width = data.width;
    canvas.height = data.height;
    const context = canvas.getContext("2d");
    const image = context.createImageData(data.width, data.height);
    const f = data.fields;
    const s = data.stats;
    const heat = activeHeat(data, selected);
    for (let y = 0; y < data.height; y += 1) {
      for (let x = 0; x < data.width; x += 1) {
        const index = y * data.width + x;
        const west = y * data.width + (x + data.width - 1) % data.width;
        const east = y * data.width + (x + 1) % data.width;
        const north = ((y + data.height - 1) % data.height) * data.width + x;
        const south = ((y + 1) % data.height) * data.width + x;
        const water = isWater(data, index);
        let color = water ? [35, 62, 77] : [128, 143, 126];
        if (selected.biomes && data.available.biomes) {
          color = [...(climateColors[Math.round(f.climateZone[index])] || [136, 148, 131])];
          if (!showIce && ice && ice[index] === 1) {
            color = selected.heights && f.elevation
              ? elevationColor(f.elevation[index], s.elevation) : [...climateColors[4]];
          }
        } else if (selected.heights && data.available.heights) {
          color = elevationColor(f.elevation[index], s.elevation);
        }
        if (selected.heights && data.available.heights) {
          const range = s.elevation.maximum - s.elevation.minimum || 1;
          const slope = (f.elevation[west] - f.elevation[east])
            + (f.elevation[north] - f.elevation[south]) * 0.65;
          const shade = clamp(0.78 + fraction(f.elevation[index], s.elevation) * 0.34
            + slope * 5 / range, 0.48, 1.3);
          color = color.map((channel) => channel * shade);
          if (contour(f.elevation, index, west, north, s.elevation.step)) {
            mix(color, water ? [105, 151, 169] : [235, 222, 186], water ? 0.19 : 0.29);
          }
        }
        if (water !== isWater(data, west) || water !== isWater(data, north)) {
          mix(color, water ? [91, 143, 158] : [191, 190, 147], 0.58);
        }
        if (f.riverMask && f.riverMask[index]) mix(color, [81, 145, 172], 0.8);
        if (showIce && ice && ice[index]) {
          mix(color, ice[index] === 2 ? [203, 227, 231] : [236, 242, 235], ice[index] === 2 ? 0.82 : 0.3);
        }
        if (heat.length) {
          color = color.map((channel) => clamp(channel, 0, 255));
          const tint = [0, 0, 0];
          let count = 0;
          heat.forEach((id) => {
            if (!Number.isFinite(f[id][index])) return;
            const current = heatColor(id, f[id][index], s[id]);
            for (let channel = 0; channel < 3; channel += 1) tint[channel] += current[channel];
            count += 1;
          });
          if (count) {
            mix(color, tint.map((channel) => channel / count), heatAlpha(count));
          }
        }
        if (selected.plates && data.available.plates
          && (f.plateId[index] !== f.plateId[west] || f.plateId[index] !== f.plateId[north])) {
          mix(color, [211, 153, 233], 0.95);
        }
        const offset = index * 4;
        image.data[offset] = color[0];
        image.data[offset + 1] = color[1];
        image.data[offset + 2] = color[2];
        image.data[offset + 3] = 255;
      }
    }
    context.putImageData(image, 0, 0);
    data.rasters.set(key, canvas);
    if (data.rasters.size > CACHE_LIMIT) data.rasters.delete(data.rasters.keys().next().value);
    return canvas;
  }

  function atlasData(atlas) {
    if (!atlas || typeof atlas !== "object") return null;
    if (!caches.has(atlas)) caches.set(atlas, prepare(atlas));
    return caches.get(atlas);
  }

  function getIceMask(atlas) {
    const data = atlasData(atlas);
    if (!data) return null;
    if (data.iceMask !== undefined) return data.iceMask;
    const fields = data.fields;
    if (!fields.climateZone || !fields.temperature || !fields.oceanMask || !fields.lakeMask) {
      data.iceMask = null;
      return null;
    }
    const mask = new Uint8Array(data.width * data.height);
    let land = 0;
    let water = 0;
    for (let index = 0; index < mask.length; index += 1) {
      const isWater = Boolean(fields.oceanMask[index] || fields.lakeMask[index]);
      if (isWater && fields.temperature[index] <= -10) {
        mask[index] = 2;
        water += 1;
      } else if (!isWater && fields.climateZone[index] === 5) {
        mask[index] = 1;
        land += 1;
      }
    }
    data.iceMask = mask;
    data.iceCounts = { land, water };
    return mask;
  }

  function getRaster(atlas, selected = createState().layers, options = {}) {
    const data = atlasData(atlas);
    return data ? raster(data, selected, options) : null;
  }

  function arrow(context, x, y, u, v, length, color, doubleHead) {
    const magnitude = Math.hypot(u, v);
    if (!Number.isFinite(magnitude)) return;
    if (magnitude < 1e-10) {
      context.fillStyle = color;
      context.fillRect(Math.round(x), Math.round(y), 2, 2);
      return;
    }
    const dx = u / magnitude;
    const dy = v / magnitude;
    const endX = x + dx * length * 0.5;
    const endY = y + dy * length * 0.5;
    context.beginPath();
    context.moveTo(x - dx * length * 0.5, y - dy * length * 0.5);
    context.lineTo(endX, endY);
    const heads = doubleHead ? [0, 4] : [0];
    heads.forEach((offset) => {
      const hx = endX - dx * offset;
      const hy = endY - dy * offset;
      context.moveTo(hx - dx * 4 - dy * 3, hy - dy * 4 + dx * 3);
      context.lineTo(hx, hy);
      context.lineTo(hx - dx * 4 + dy * 3, hy - dy * 4 - dx * 3);
    });
    context.strokeStyle = "#112a35";
    context.lineWidth = 3.5;
    context.stroke();
    context.strokeStyle = color;
    context.lineWidth = 1.5;
    context.stroke();
  }

  function drawVectors(api, data, selected, left, top, size, compact, viewport) {
    const f = data.fields;
    const step = Math.max(3, Math.round(data.width * (compact ? 41 : 53) / size));
    for (const [id, uName, vName, color, offset] of [
      ["wind", "windU", "windV", "#63dbd4", 0.25],
      ["currents", "currentU", "currentV", "#dceef7", 0.7],
    ]) {
      if (!selected[id] || !data.available[id]) continue;
      for (let y = Math.floor(step * offset); y < data.height; y += step) {
        for (let x = Math.floor(step * offset); x < data.width; x += step) {
          const index = y * data.width + x;
          if (id === "currents" && f.oceanMask && !f.oceanMask[index]) continue;
          const px = left + (x + 0.5) / data.width * size;
          const py = top + (y + 0.5) / data.height * size;
          if (px < viewport.x - 20 || px > viewport.x + viewport.width + 20
            || py < viewport.y - 20 || py > viewport.y + viewport.height + 20) continue;
          arrow(api.context, px, py, f[uName][index], f[vName][index], compact ? 13 : 18,
            color, id === "currents");
        }
      }
    }
    if (!selected.plates || !data.available.plates) return;
    data.plateCenters.forEach((plate) => {
      const x = left + plate.x * size;
      const y = top + plate.y * size;
      if (x < viewport.x - 32 || x > viewport.x + viewport.width + 32
        || y < viewport.y - 32 || y > viewport.y + viewport.height + 32) return;
      if (f.plateVx && f.plateVy) {
        arrow(api.context, x, y - 10, f.plateVx[plate.index], f.plateVy[plate.index],
          compact ? 17 : 24, "#d5a4ed", false);
      }
      api.outlinedText(`P${plate.id}`, x - 8, y + 4, 1, "#ebc9f8", "#25313f");
    });
  }

  function sample(data, name) {
    if (!data || !data.fields[name]) return NaN;
    return data.fields[name][Math.floor(PLAYER.y * data.height) * data.width
      + Math.floor(PLAYER.x * data.width)];
  }

  function layerStatus(data, state, id) {
    return !data || !data.available[id] ? "unavailable" : state.layers[id] ? "on" : "off";
  }

  function range(data, id) {
    const stats = data.stats[id];
    const minimumPlaces = stats.minimum > 0 && stats.minimum < 0.01 ? 4 : 2;
    return `${number(stats.minimum, minimumPlaces)}..${number(stats.maximum, stats.maximum < 1 ? 2 : 1)}`;
  }

  function drawHeatKeys(api, data, heat) {
    const columnWidth = (api.width - 24) / heat.length;
    heat.forEach((id, index) => {
      const layer = layers.find((item) => item.id === id);
      const stats = data.stats[id];
      const x = 12 + index * columnWidth;
      const label = `${layer.key} ${range(data, id)} ${id === "temperature" ? "C" : "model"}`;
      api.outlinedText(label, x, 56, 1, "#e7eadb");
      const barX = x + label.length * 8 + 8;
      for (let pixel = 0; pixel < 48; pixel += 1) {
        const position = pixel / 47;
        const value = stats.minimum + (id === "precipitation"
          ? Math.expm1(Math.log1p(stats.maximum - stats.minimum) * position)
          : (stats.maximum - stats.minimum) * position);
        const color = heatColor(id, value, stats);
        api.context.fillStyle = `rgb(${color.map(Math.round).join(",")})`;
        api.context.fillRect(barX + pixel, 60, 1, 8);
      }
      if (id === "precipitation") api.outlinedText("log", barX + 54, 56, 1, "#d4c5e6");
    });
  }

  function detail(data, layer) {
    if (!data || !data.available[layer.id]) return ["", "Source unavailable; no values supplied"];
    const value = (name) => number(sample(data, name));
    if (layer.id === "plates") return [`ID ${value("plateId")}`, "Edges; model world/epoch drift arrows"];
    if (layer.id === "heights") return [`${value("elevation")} km`, `${number(data.stats.elevation.step)} km contours / ${range(data, "elevation")} km / sea 0`];
    if (layer.id === "temperature") return [`${value("temperature")} C`, `Translucent field / ${range(data, "temperature")} C`];
    if (layer.id === "biomes") {
      const zone = Math.round(sample(data, "climateZone"));
      const names = data.atlas.climateZoneNames || climateNames;
      return [names[zone] || `Zone ${zone}`, "Coarse climate-zone proxy"];
    }
    if (layer.id === "wind") return ["", "Cyan single arrows; cells/climate step"];
    if (layer.id === "currents") return ["", "White double arrows; cells/climate step"];
    if (layer.id === "moisture") return [`${value("moisture")} model`, `Reservoir ${range(data, "moisture")} / linear heat`];
    return [`${value("precipitation")} model`, `Accum rain ${range(data, "precipitation")} / log heat`];
  }

  function drawDetails(api, state, data, heat) {
    const x = 10;
    const y = heat.length ? 84 : 60;
    const width = Math.min(api.width - 20, 1040);
    const columnWidth = (width - 30) / 2;
    api.context.fillStyle = "rgba(12,29,36,0.84)";
    api.context.fillRect(x, y, width, 232);
    const source = data ? ` / SEED ${data.atlas.seed ?? "--"} / ${data.width}x${data.height}` : " / SOURCE UNAVAILABLE";
    api.text(`LAYER DETAILS / L Close${source}`, x + 10, y + 8, 1, "#e7e2cc");
    const iceRule = data && getIceMask(data.atlas)
      ? "ICE PROXY: Snow land / water <= -10 C" : "ICE PROXY: source unavailable";
    api.text(iceRule, x + width - iceRule.length * 8 - 10, y + 8, 1, "#b5d8dc");
    layers.forEach((layer, index) => {
      const column = Math.floor(index / 4);
      const row = index % 4;
      const rowX = x + 10 + column * (columnWidth + 10);
      const rowY = y + 32 + row * 38;
      const status = layerStatus(data, state, layer.id);
      const label = status === "unavailable" ? "N/A" : status.toUpperCase();
      const info = detail(data, layer);
      api.text(`${layer.key} ${layer.name} ${label}${info[0] ? ` / ${info[0]}` : ""}`,
        rowX, rowY, 1, status === "on" ? layer.color : "#bbc5be");
      api.fitText(info[1], rowX, rowY + 18, 1, columnWidth, "#97adb1");
    });
    const note = data && data.iceCounts
      ? `Ice proxy: ${data.iceCounts.land} land / ${data.iceCounts.water} water. Climate view, not freezing physics. Arrows: direction only.`
      : "Ice proxy unavailable. Arrows show direction, not speed.";
    api.fitText(note, x + 10, y + 186, 1, width - 20, "#aabdb7");
    if (data && data.available.biomes) {
      api.text("B:", x + 10, y + 208, 1, "#9eb1b3");
      let cursor = x + 38;
      climateNames.forEach((name, index) => {
        api.context.fillStyle = `rgb(${climateColors[index].join(",")})`;
        api.context.fillRect(cursor, y + 212, 8, 8);
        api.text(name, cursor + 12, y + 208, 1, "#b9c6bb");
        cursor += 24 + name.length * 8;
      });
    }
    if (data && data.available.heights) {
      let cursor = x + 498;
      api.text("H km:", cursor, y + 208, 1, "#9eb1b3");
      cursor += 48;
      const stats = data.stats.elevation;
      [stats.minimum, 0, stats.maximum].forEach((value) => {
        const label = number(value, 1);
        api.context.fillStyle = `rgb(${elevationColor(value, stats).map(Math.round).join(",")})`;
        api.context.fillRect(cursor, y + 212, 8, 8);
        api.text(label, cursor + 12, y + 208, 1, "#b9c6bb");
        cursor += 24 + label.length * 8;
      });
    }
  }

  function drawOverlay(api, state, data, heat) {
    const { context, width, height } = api;
    context.fillStyle = "rgba(12,29,36,0.62)";
    context.fillRect(6, 6, width - 12, heat.length ? 70 : 46);
    const view = state.overview ? "Whole world" : `${state.zoom.toFixed(2)}x / Player view`;
    const opacity = heat.length ? ` / Heat ${Math.round(heatAlpha(heat.length) * 100)}%${heat.length > 1 ? " mixed" : ""}` : "";
    api.outlinedText(`MAP N^ / ${view}${opacity}`, 12, 8, 1, "#f0e7cc");
    const help = `0 Whole world  R Reset  L ${state.legendExpanded ? "Hide details" : "Details"}`;
    api.outlinedText(help, width - help.length * 8 - 12, 8, 1, "#d4ddd0");
    const shortNames = ["Plate", "Height", "Temp", "Biomes", "Wind", "Curr", "Moist", "Rain"];
    let cursor = 12;
    layers.forEach((layer, index) => {
      const status = layerStatus(data, state, layer.id);
      const label = status === "unavailable" ? "N/A" : status.toUpperCase();
      api.outlinedText(`${layer.key} ${shortNames[index]} ${label}`, cursor, 32, 1,
        status === "on" ? layer.color : "#93a7aa");
      cursor += (shortNames[index].length + 6) * 8 + 12;
    });
    if (heat.length) drawHeatKeys(api, data, heat);
    if (state.legendExpanded) drawDetails(api, state, data, heat);
    context.fillStyle = "rgba(12,29,36,0.62)";
    context.fillRect(6, height - 26, width - 12, 20);
    api.outlinedText("Arrows pan  Shift faster  +/- zoom  M/Esc Close", 12, height - 24, 1, "#d7dfcd");
  }

  function draw(api, state) {
    const { context, width, height } = api;
    const compact = height < 500 || width < 1080;
    const atlas = api.atlas || window.S2_WORLD_DATA;
    const data = atlasData(atlas);
    const heat = activeHeat(data, state.layers);
    const tileSize = state.overview ? Math.min(width, height) : Math.max(width, height) * state.zoom;
    const originX = width / 2 - tileSize / 2 - (state.overview ? 0 : state.panX * tileSize);
    const originY = height / 2 - tileSize / 2 - (state.overview ? 0 : state.panY * tileSize);
    const viewport = state.overview
      ? { x: originX, y: originY, width: tileSize, height: tileSize }
      : { x: 0, y: 0, width, height };
    context.fillStyle = "#15272f";
    context.fillRect(0, 0, width, height);
    context.save();
    context.beginPath();
    context.rect(viewport.x, viewport.y, viewport.width, viewport.height);
    context.clip();
    context.imageSmoothingEnabled = false;
    const mapRaster = getRaster(atlas, state.layers) || api.mapImage;
    const imageReady = data || (mapRaster && mapRaster.complete && mapRaster.naturalWidth > 0);
    if (imageReady) {
      const tiles = state.overview ? [0] : [-1, 0, 1];
      tiles.forEach((row) => {
        tiles.forEach((column) => {
          const left = originX + column * tileSize;
          const top = originY + row * tileSize;
          if (left >= width || left + tileSize <= 0 || top >= height || top + tileSize <= 0) return;
          if (data) context.drawImage(mapRaster, left, top, tileSize, tileSize);
          else context.drawImage(mapRaster, 0, 0, 256, 256, left, top, tileSize, tileSize);
          context.strokeStyle = "rgba(223,236,218,0.09)";
          context.lineWidth = 1;
          context.beginPath();
          for (let division = 1; division < 4; division += 1) {
            context.moveTo(left + tileSize * division / 4, top);
            context.lineTo(left + tileSize * division / 4, top + tileSize);
            context.moveTo(left, top + tileSize * division / 4);
            context.lineTo(left + tileSize, top + tileSize * division / 4);
          }
          context.stroke();
          if (data) drawVectors(api, data, state.layers, left, top, tileSize, compact, viewport);
          const playerX = left + PLAYER.x * tileSize;
          const playerY = top + PLAYER.y * tileSize;
          api.outlinedText("+", playerX - 8, playerY - 16, 2, "#ffe49a", "#18303b");
          api.outlinedText("YOU", playerX + 12, playerY + 8, 1, "#ffe49a", "#18303b");
        });
      });
    } else {
      api.outlinedText("Map source unavailable", 12, height / 2, 1, "#d5c9ac");
    }
    context.restore();
    drawOverlay(api, state, data, heat);
    return {
      layers: Object.fromEntries(layers.map((layer) => [layer.id, layerStatus(data, state, layer.id)])),
      view: state.overview ? "whole-world" : "close",
      legendExpanded: Boolean(state.legendExpanded),
    };
  }

  window.S2WorldMapView = { createState, handleKey, getRaster, getIceMask, draw };
})();
