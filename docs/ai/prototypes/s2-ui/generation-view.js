(() => {
  "use strict";

  const stages = [
    { label: "Relief", caption: "Raising land relief" },
    { label: "Water", caption: "Filling basins and rivers" },
    { label: "Climate", caption: "Forming climate zones" },
    { label: "Arrival", caption: "Locating the starting area" },
  ];
  const descriptions = [
    ["Ridges rise.", "Valleys deepen."],
    ["Oceans settle.", "Rivers connect."],
    ["Cold highlands.", "Warm lowlands."],
    ["A whole world.", "One starting point."],
  ];
  const zoneColors = [
    [48, 112, 135], [172, 148, 91], [58, 111, 65],
    [98, 143, 77], [135, 151, 127], [229, 237, 224],
  ];
  const raster = document.createElement("canvas");
  const rasterContext = raster.getContext("2d");
  let cached = null;

  const clamp = (value, low = 0, high = 1) => Math.max(low, Math.min(high, value));
  const ease = (value) => value * value * (3 - 2 * value);
  const mix = (from, to, amount) => from.map((value, index) => value + (to[index] - value) * amount);
  const channel = (atlas, name) => atlas[name] && atlas[name].length === atlas.width * atlas.height;

  function prepare(atlas, availableWidth) {
    if (!atlas || !Number.isInteger(atlas.width) || !Number.isInteger(atlas.height)
      || atlas.width < 2 || atlas.height < 2 || !channel(atlas, "elevation")) return null;
    const limit = Math.max(24, Math.min(128, Math.floor((availableWidth - 24) / 3)));
    const width = Math.min(atlas.width, limit);
    const height = Math.min(atlas.height, limit);
    if (cached && cached.atlas === atlas && cached.width === width && cached.height === height) return cached;

    const samples = [];
    let lowest = Infinity;
    let highest = -Infinity;
    for (let y = 0; y < height; y += 1) {
      const sourceY = Math.round(y * (atlas.height - 1) / (height - 1));
      for (let x = 0; x < width; x += 1) {
        const sourceX = Math.round(x * (atlas.width - 1) / (width - 1));
        const index = sourceY * atlas.width + sourceX;
        const elevation = atlas.elevation[index];
        if (!Number.isFinite(elevation)) return null;
        lowest = Math.min(lowest, elevation);
        highest = Math.max(highest, elevation);
        samples.push({ index, elevation });
      }
    }
    const range = Math.max(0.001, highest - lowest);
    const hasWater = channel(atlas, "waterLevel");
    const hasZones = channel(atlas, "climateZone");
    const waterMasks = ["oceanMask", "lakeMask", "riverMask"].map((name) => channel(atlas, name));
    const relief = Math.max(18, Math.round(height * 0.3));
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) {
        const sample = samples[y * width + x];
        const { index, elevation } = sample;
        const west = samples[y * width + Math.max(0, x - 1)].elevation;
        const east = samples[y * width + Math.min(width - 1, x + 1)].elevation;
        const north = samples[Math.max(0, y - 1) * width + x].elevation;
        const south = samples[Math.min(height - 1, y + 1) * width + x].elevation;
        sample.level = (elevation - lowest) / range;
        sample.shade = clamp(1 + ((west - east) + (north - south) * 1.5) / range * 3, 0.58, 1.24);
        sample.rock = mix([97, 87, 72], [187, 174, 137], sample.level);
        sample.zone = hasZones ? zoneColors[atlas.climateZone[index]] : null;
        sample.waterKind = waterMasks[0] && atlas.oceanMask[index] ? 1
          : waterMasks[1] && atlas.lakeMask[index] ? 2
            : waterMasks[2] && atlas.riverMask[index] ? 3 : 0;
        sample.water = hasWater && Number.isFinite(atlas.waterLevel[index])
          ? atlas.waterLevel[index] : null;
      }
    }
    raster.width = width * 2 + height + 24;
    raster.height = height + relief + 24;
    cached = { atlas, width, height, samples, lowest, range, relief,
      hasWater: hasWater && waterMasks.some(Boolean), hasZones, frameKey: "" };
    return cached;
  }

  function paint(field, stageIndex, progress) {
    const frameKey = `${stageIndex}:${Math.round(progress * 90)}`;
    if (field.frameKey === frameKey) return;
    field.frameKey = frameKey;
    const { width, height, samples, lowest, range, relief } = field;
    const uplift = stageIndex === 0 ? 0.06 + ease(progress) * 0.94 : 1;
    const waterProgress = stageIndex < 1 ? 0 : stageIndex === 1 ? ease(progress) : 1;
    const climateProgress = stageIndex < 2 ? 0 : stageIndex === 2 ? ease(progress) : 1;
    const image = rasterContext.createImageData(raster.width, raster.height);
    const pixels = image.data;

    // Back-to-front columns expose the actual height field as a voxel-like relief.
    for (let y = 0; y < height; y += 1) {
      for (let x = 0; x < width; x += 1) {
        const sample = samples[y * width + x];
        let level = sample.level * uplift;
        let color = sample.zone ? mix(sample.rock, sample.zone, climateProgress) : sample.rock;
        let shade = sample.shade;
        const waterLevel = sample.water === null ? -1
          : (sample.water - lowest) / range * waterProgress;
        const wet = sample.waterKind && waterProgress > 0
          && waterLevel >= level - (waterProgress === 1 ? 0.002 : 0);
        if (wet) {
          const depth = clamp((waterLevel - level) * 4);
          level = Math.max(level, waterLevel);
          color = sample.waterKind === 3 ? [100, 166, 172]
            : mix([65, 139, 153], [29, 69, 94], depth);
          shade = 1;
        }
        const screenX = 12 + x * 2 + y;
        const datumY = 8 + relief + y;
        const topY = datumY - Math.round(level * relief);
        const bottomY = datumY + 8;
        for (let screenY = topY; screenY <= bottomY; screenY += 1) {
          const top = screenY === topY;
          const brightness = top ? shade : wet ? 0.57 : 0.46;
          for (let offset = 0; offset < 3; offset += 1) {
            const pixel = (screenY * raster.width + screenX + offset) * 4;
            pixels[pixel] = color[0] * brightness;
            pixels[pixel + 1] = color[1] * brightness;
            pixels[pixel + 2] = color[2] * brightness;
            pixels[pixel + 3] = 255;
          }
        }
      }
    }
    rasterContext.putImageData(image, 0, 0);
  }

  function drawMarker(api, field, bounds, player) {
    if (!player || !Number.isFinite(player.x) || !Number.isFinite(player.y)) return;
    const x = Math.round(clamp(player.x) * (field.width - 1));
    const y = Math.round(clamp(player.y) * (field.height - 1));
    const sample = field.samples[y * field.width + x];
    const water = sample.waterKind && sample.water !== null
      ? (sample.water - field.lowest) / field.range : sample.level;
    const markerX = bounds.x + (12 + x * 2 + y + 1) * bounds.scale;
    const markerY = bounds.y + (8 + field.relief + y
      - Math.round(Math.max(sample.level, water) * field.relief)) * bounds.scale;
    const { context } = api;
    const unit = Math.max(1, bounds.scale);
    context.fillStyle = "#172522";
    context.fillRect(markerX - unit * 2, markerY - unit * 15, unit * 5, unit * 15);
    context.fillStyle = api.colors.accent;
    context.fillRect(markerX, markerY - unit * 14, unit, unit * 14);
    context.fillRect(markerX + unit, markerY - unit * 14, unit * 7, unit * 4);
    api.outlinedText("START", markerX + unit * 10, markerY - unit * 15,
      1, api.colors.accent);
  }

  function draw(api, model) {
    const { context, width, height, colors } = api;
    const margin = width < 640 ? 16 : 28;
    const wide = width >= 760 && height >= 320;
    const titleScale = width >= 1100 && height >= 520 ? 3 : width >= 360 ? 2 : 1;
    const bodyScale = wide ? 2 : 1;
    const phase = model.phase || "sample";
    const stageIndex = phase === "ready" ? stages.length - 1
      : clamp(Math.floor(model.stageIndex || 0), 0, stages.length - 1);
    const progress = phase === "ready" || model.reducedMotion ? 1
      : clamp(Number.isFinite(model.stageProgress) ? model.stageProgress : 0);
    const bodyTop = margin + titleScale * 16 + (wide ? 52 : 44);
    const sideWidth = width >= 1100 ? 300 : 260;
    const fieldX = wide ? margin + sideWidth + 28 : margin;
    const fieldWidth = width - fieldX - margin;
    const fieldY = wide ? bodyTop : bodyTop + 90;
    const fieldBottom = height - margin - (wide ? 66 : 72);
    const field = prepare(api.atlas || window.S2_WORLD_DATA, fieldWidth);

    context.fillStyle = "#172522";
    context.fillRect(0, 0, width, height);
    api.text("Creating your world", margin, margin, titleScale, colors.light);
    api.text(`${String(stageIndex + 1).padStart(2, "0")} / 04`, margin, bodyTop,
      1, colors.quiet);
    api.text(stages[stageIndex].label, margin, bodyTop + 24, titleScale, colors.accent);
    const descriptionY = bodyTop + 28 + titleScale * 16;
    descriptions[stageIndex].forEach((line, index) => {
      api.text(line, margin, descriptionY + index * (bodyScale * 16 + 6), bodyScale, colors.light);
    });

    if (wide && height >= 560) {
      const listScale = 2;
      const listY = Math.max(descriptionY + bodyScale * 38 + 28,
        fieldBottom - stages.length * (listScale * 16 + 12));
      stages.forEach((stage, index) => {
        const prefix = index < stageIndex ? "+ " : index === stageIndex ? "> " : "  ";
        api.text(prefix + stage.label, margin, listY + index * (listScale * 16 + 12), listScale,
          index === stageIndex ? colors.accent : index < stageIndex ? colors.light : colors.quiet);
      });
    }

    if (field) {
      paint(field, stageIndex, progress);
      const scale = Math.max(1, Math.floor(Math.min(fieldWidth / raster.width,
        Math.max(1, fieldBottom - fieldY) / raster.height)));
      const x = Math.round(fieldX + (fieldWidth - raster.width * scale) / 2);
      const y = Math.round(fieldY + Math.max(0, (fieldBottom - fieldY - raster.height * scale) / 2));
      const smoothing = context.imageSmoothingEnabled;
      context.imageSmoothingEnabled = false;
      context.drawImage(raster, x, y, raster.width * scale, raster.height * scale);
      context.imageSmoothingEnabled = smoothing;
      if (stageIndex === 3) drawMarker(api, field, { x, y, scale }, model.player);
      api.text(`Seed ${field.atlas.seed === undefined ? "?" : field.atlas.seed}`,
        fieldX, Math.max(bodyTop, y - 24), 1, colors.quiet);
    } else {
      api.text("World data unavailable", fieldX, fieldY + 48, 1, colors.light);
      api.text("The world preview cannot be drawn.", fieldX, fieldY + 72, 1, colors.quiet);
    }

    let status = !field ? "World data unavailable"
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
    } else if (field && stageIndex === 1 && !field.hasWater) {
      status = "Water data unavailable";
    } else if (field && stageIndex === 2 && !field.hasZones) {
      status = "Climate zones unavailable";
    } else if (field && stageIndex === 2) {
      status = "Forming coarse climate zones";
    }
    const footerScale = phase === "ready" && width >= 760 ? 2 : 1;
    api.text(status, margin, height - margin - footerScale * 32 - 8, footerScale, statusColor);
    api.text(action, margin, height - margin - footerScale * 16, footerScale, colors.accent);
    if (phase === "ready") api.text("Esc  Cancel", width - margin - 88,
      height - margin - 16, 1, colors.quiet);
  }

  window.S2GenerationView = { stages, durationMs: 16000, draw };
})();
