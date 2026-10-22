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
    { id: "moisture", key: "O", name: "MOISTURE", color: "#7ca7de", fields: ["moisture"] },
    { id: "precipitation", key: "Q", name: "PRECIPITATION", color: "#d9d0ed", fields: ["precipitation"] },
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
      zoom: 1, panX: 0, panY: 0,
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
    const step = (shiftKey ? 0.24 : 0.08) / state.zoom;
    if (key === "ArrowLeft") state.panX -= step;
    else if (key === "ArrowRight") state.panX += step;
    else if (key === "ArrowUp") state.panY -= step;
    else if (key === "ArrowDown") state.panY += step;
    else if (key === "+" || key === "=") state.zoom = Math.min(6, state.zoom + 0.25);
    else if (key === "-" || key === "_") state.zoom = Math.max(1, state.zoom - 0.25);
    else if (lowerKey === "r") {
      state.zoom = 1;
      state.panX = 0;
      state.panY = 0;
    } else return false;
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

  function raster(data, selected) {
    const key = ["plates", "heights", "temperature", "biomes", "moisture", "precipitation"]
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
        } else if (selected.temperature && data.available.temperature) {
          color = temperatureColor(f.temperature[index]);
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
        if (selected.moisture && data.available.moisture) {
          const value = fraction(f.moisture[index], s.moisture);
          const line = contour(f.moisture, index, west, north, s.moisture.step);
          if ((line && (x + y) % 3 !== 0) || (x % 7 === 0 && y % 7 === 0 && value > 0.25)) {
            mix(color, [122, 167, 225], line ? 0.7 : 0.5);
          }
        }
        if (selected.precipitation && data.available.precipitation) {
          const value = fraction(f.precipitation[index], s.precipitation);
          const density = ((x * 17 + y * 29) % 97) / 97;
          if (x % 3 === 0 && y % 3 === 0 && density < value * 0.85) {
            mix(color, [225, 214, 242], 0.8);
          }
        }
        if (selected.temperature && data.available.temperature) {
          const line = contour(f.temperature, index, west, north, s.temperature.step);
          if (line || (x % 11 === 0 && y % 11 === 0)) {
            const tint = temperatureColor(f.temperature[index]);
            mix(color, tint, line ? 0.86 : 0.65);
            if (line) mix(color, [244, 231, 197], 0.22);
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

  function getRaster(atlas, selected = createState().layers) {
    const data = atlasData(atlas);
    return data ? raster(data, selected) : null;
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
          if (px < viewport.x - 20 || px > viewport.x + viewport.size + 20
            || py < viewport.y - 20 || py > viewport.y + viewport.size + 20) continue;
          arrow(api.context, px, py, f[uName][index], f[vName][index], compact ? 13 : 18,
            color, id === "currents");
        }
      }
    }
    if (!selected.plates || !data.available.plates) return;
    data.plateCenters.forEach((plate) => {
      const x = left + plate.x * size;
      const y = top + plate.y * size;
      if (x < viewport.x - 32 || x > viewport.x + viewport.size + 32
        || y < viewport.y - 32 || y > viewport.y + viewport.size + 32) return;
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

  function description(layer, data) {
    if (!data || !data.available[layer.id]) return ["Source unavailable", "No values supplied"];
    const value = (name) => number(sample(data, name));
    if (layer.id === "plates") return ["Plate edges and drift", "Arrows: drift direction"];
    if (layer.id === "heights") return [`Relief; ${number(data.stats.elevation.step)} km contours`, `YOU ${value("elevation")} km; sea = 0`];
    if (layer.id === "temperature") return [`Isotherms; ${number(data.stats.temperature.step)} C interval`, `YOU ${value("temperature")} C`];
    if (layer.id === "biomes") {
      const zone = Math.round(sample(data, "climateZone"));
      const names = data.atlas.climateZoneNames || climateNames;
      return ["Coarse climate-zone proxy", `YOU ${names[zone] || `Zone ${zone}`}`];
    }
    if (layer.id === "wind") return ["Cyan single arrows", "Airflow direction"];
    if (layer.id === "currents") return ["White double arrows", "Ocean flow direction"];
    if (layer.id === "moisture") return ["Dashed reservoir contours", `YOU ${value("moisture")} model units`];
    return ["Accumulated rain; stipple", `YOU ${value("precipitation")} model units`];
  }

  function drawLegend(api, state, data, rect, compact) {
    const columnWidth = compact ? Math.floor((rect.width - 14) / 2) : rect.width;
    const rowHeight = compact ? 50 : 48;
    layers.forEach((layer, index) => {
      const column = compact ? Math.floor(index / 4) : 0;
      const row = compact ? index % 4 : index;
      const x = rect.x + column * (columnWidth + 14);
      const y = rect.y + row * rowHeight;
      const available = Boolean(data && data.available[layer.id]);
      const enabled = Boolean(state.layers[layer.id] && available);
      const status = !available ? "N/A" : enabled ? "ON" : "OFF";
      const color = enabled ? layer.color : "#9aa9aa";
      api.text(layer.key, x, y, 2, enabled ? layer.color : "#c6cfc7");
      api.context.fillStyle = enabled ? layer.color : "#455a61";
      api.context.fillRect(x, y + 34, 16, 2);
      const labelX = x + (compact ? 28 : 40);
      api.text(layer.name, labelX, y, compact ? 1 : 2, enabled ? "#f0ead7" : "#a5b3af");
      const statusScale = compact ? 1 : 2;
      api.text(status, x + columnWidth - status.length * 8 * statusScale, y,
        statusScale, enabled ? layer.color : "#7f9398");
      const detail = description(layer, data);
      if (compact) {
        api.fitText(detail[0], labelX, y + 18, 1, columnWidth - 28, color);
        api.fitText(detail[1], labelX, y + 34, 1, columnWidth - 28, "#8da1a4");
      } else {
        api.fitText(`${detail[0]} / ${detail[1]}`, labelX, y + 32, 1,
          columnWidth - 40, color);
      }
    });
  }

  function drawClimateKey(api, x, y) {
    api.text("B:", x, y, 1, "#9eb1b3");
    let cursor = x + 28;
    climateNames.forEach((name, index) => {
      api.context.fillStyle = `rgb(${climateColors[index].join(",")})`;
      api.context.fillRect(cursor, y + 4, 8, 8);
      api.text(name, cursor + 12, y, 1, "#b9c6bb");
      cursor += 12 + name.length * 8 + 12;
    });
  }

  function drawScalarKey(api, data, id, x, y) {
    const thermal = id === "temperature";
    const stats = data.stats[thermal ? "temperature" : "elevation"];
    const values = thermal
      ? [stats.minimum, ...[-20, 0, 20].filter((value) => value > stats.minimum + 1 && value < stats.maximum - 1), stats.maximum]
      : [stats.minimum, ...(stats.minimum < 0 && stats.maximum > 0 ? [0] : []), stats.maximum];
    api.text(thermal ? "T:" : "H:", x, y, 1, "#9eb1b3");
    let cursor = x + 28;
    values.forEach((value, index) => {
      const color = thermal ? temperatureColor(value) : elevationColor(value, stats);
      const unit = thermal ? "C" : "km";
      let label = `${number(value, 1)} ${unit}`;
      if (thermal && index === 0) label = `Cold ${label}`;
      if (thermal && index === values.length - 1) label += " Warm";
      if (!thermal && value === 0) label = "Sea 0 km";
      api.context.fillStyle = `rgb(${color.map(Math.round).join(",")})`;
      api.context.fillRect(cursor, y + 4, 8, 8);
      api.text(label, cursor + 12, y, 1, "#b9c6bb");
      cursor += 12 + label.length * 8 + 12;
    });
  }

  function draw(api, state) {
    const { context, width, height } = api;
    const compact = height < 500 || width < 1080;
    const margin = compact ? 6 : 8;
    const mapTop = 24;
    const gap = compact ? 16 : 20;
    const mapSize = Math.max(1, Math.floor(Math.min(height - mapTop - 20,
      width - margin * 2 - gap - (compact ? 480 : 500))));
    const mapRect = { x: margin, y: mapTop, size: mapSize };
    const legendX = margin + mapSize + gap;
    const legendWidth = Math.max(1, width - legendX - margin);
    const atlas = api.atlas || window.S2_WORLD_DATA;
    const data = atlasData(atlas);
    const biomeBase = Boolean(data && data.available.biomes && state.layers.biomes);
    const temperatureLayer = Boolean(data && data.available.temperature && state.layers.temperature);
    const heightRelief = Boolean(data && data.available.heights && state.layers.heights);
    const baseLabel = !data ? "unavailable" : biomeBase ? "climate zones" : temperatureLayer ? "temperature"
      : heightRelief ? "elevation" : "land / water";
    const composition = `${baseLabel}${biomeBase && temperatureLayer ? " + isotherms" : ""}${heightRelief ? " + relief" : ""}`;
    context.fillStyle = "#15272f";
    context.fillRect(0, 0, width, height);
    api.text("WORLD ATLAS", margin, 4, 1, "#f1e8cc");
    api.text("LAYERS / press a key to turn it on or off", legendX, 4, 1, "#c2cfc4");
    const sourceLabel = data
      ? `SEED ${data.atlas.seed ?? "--"} / ${data.width}x${data.height}`
      : "MAP DATA UNAVAILABLE";
    api.fitText(sourceLabel, margin + 112, 4, 1, Math.max(1, mapSize - 112), "#91a7aa");
    const zoomLabel = `${state.zoom.toFixed(2)}x`;
    context.save();
    context.beginPath();
    context.rect(mapRect.x, mapRect.y, mapSize, mapSize);
    context.clip();
    context.imageSmoothingEnabled = false;
    const mapRaster = getRaster(atlas, state.layers) || api.mapImage;
    const imageReady = data || (mapRaster && mapRaster.complete && mapRaster.naturalWidth > 0);
    if (imageReady) {
      const tileSize = mapSize * state.zoom;
      const originX = mapRect.x + mapSize / 2 - tileSize / 2 - state.panX * tileSize;
      const originY = mapRect.y + mapSize / 2 - tileSize / 2 - state.panY * tileSize;
      for (let row = -1; row <= 1; row += 1) {
        for (let column = -1; column <= 1; column += 1) {
          const left = originX + column * tileSize;
          const top = originY + row * tileSize;
          if (left >= mapRect.x + mapSize || left + tileSize <= mapRect.x
            || top >= mapRect.y + mapSize || top + tileSize <= mapRect.y) continue;
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
          if (data) drawVectors(api, data, state.layers, left, top, tileSize, compact, mapRect);
          const playerX = left + PLAYER.x * tileSize;
          const playerY = top + PLAYER.y * tileSize;
          api.outlinedText("+", playerX - 8, playerY - 16, 2, "#ffe49a", "#18303b");
          api.outlinedText("YOU", playerX + 12, playerY + 8, 1, "#ffe49a", "#18303b");
        }
      }
    } else {
      context.fillStyle = "#223942";
      context.fillRect(mapRect.x, mapRect.y, mapSize, mapSize);
      api.text("Map source unavailable", mapRect.x + 12, mapRect.y + mapSize / 2, 1, "#d5c9ac");
    }
    context.restore();
    context.strokeStyle = "#6c878a";
    context.lineWidth = 1;
    context.strokeRect(mapRect.x - 1, mapRect.y - 1, mapSize + 2, mapSize + 2);
    const cardinalScale = compact ? 1 : 2;
    for (const [label, x, y] of [
      ["N", mapRect.x + mapSize / 2 - 4 * cardinalScale, mapRect.y + 6],
      ["S", mapRect.x + mapSize / 2 - 4 * cardinalScale, mapRect.y + mapSize - 16 * cardinalScale - 6],
      ["W", mapRect.x + 7, mapRect.y + mapSize / 2 - 8 * cardinalScale],
      ["E", mapRect.x + mapSize - 8 * cardinalScale - 7, mapRect.y + mapSize / 2 - 8 * cardinalScale],
    ]) api.outlinedText(label, x, y, cardinalScale, "#e6ead7", "#233b43");
    api.outlinedText(zoomLabel, mapRect.x + mapSize - zoomLabel.length * 8 - 8,
      mapTop + mapSize - 24, 1, "#e4e3ce");
    drawLegend(api, state, data, { x: legendX, y: 32, width: legendWidth }, compact);
    const compositionY = compact ? 244 : 428;
    api.fitText(`Color: ${composition}`, legendX, compositionY, 1, legendWidth, "#b9c6bb");
    if (compact) {
      if (biomeBase) drawClimateKey(api, legendX, 268);
      else if (temperatureLayer) drawScalarKey(api, data, "temperature", legendX, 268);
      else if (heightRelief) drawScalarKey(api, data, "heights", legendX, 268);
      else api.text("Land / water base; no scalar color selected", legendX, 268, 1, "#90a6aa");
      if (biomeBase && temperatureLayer) drawScalarKey(api, data, "temperature", legendX, 292);
      else api.fitText("Arrows show direction, not speed", legendX,
        292, 1, legendWidth, "#90a6aa");
    } else {
      if (biomeBase) drawClimateKey(api, legendX, 452);
      else if (temperatureLayer) drawScalarKey(api, data, "temperature", legendX, 452);
      else if (heightRelief) drawScalarKey(api, data, "heights", legendX, 452);
      else api.text("Land / water base; no scalar color selected", legendX, 452, 1, "#9eb1b3");
      if (biomeBase && temperatureLayer) drawScalarKey(api, data, "temperature", legendX, 476);
      else api.fitText("Cyan wind / white currents / purple plate motion", legendX, 476, 1, legendWidth, "#9eb1b3");
      api.fitText("Arrows show direction, not speed", legendX, 500, 1, legendWidth, "#90a6aa");
    }
    api.fitText("Arrows pan   Shift faster   +/- zoom   R center   M/Esc close", margin,
      height - 18, 1, width - margin * 2, "#d7dfcd");
    return { layers: Object.fromEntries(layers.map((layer) => [layer.id,
      !data || !data.available[layer.id] ? "unavailable" : state.layers[layer.id] ? "on" : "off",
    ])) };
  }

  window.S2WorldMapView = { createState, handleKey, getRaster, draw };
})();
