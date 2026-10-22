(() => {
  "use strict";

  const stages = [
    { label: "Relief", caption: "Mapping ridges and valleys" },
    { label: "Water", caption: "Tracing water and drainage" },
    { label: "Climate", caption: "Forming coarse climate zones" },
    { label: "Arrival", caption: "Locating the starting area" },
  ];
  const descriptions = [
    ["Ridges and valleys.", "The shape of land."],
    ["Coasts and rivers.", "Moisture fields."],
    ["Temperature.", "Climate zones."],
    ["A whole world.", "One starting point."],
  ];
  const baseLayers = {
    plates: false, heights: false, temperature: false, biomes: false,
    wind: false, currents: false, moisture: false, precipitation: false,
  };
  const layerSets = [
    baseLayers,
    { ...baseLayers, heights: true },
    { ...baseLayers, heights: true, moisture: true, precipitation: true },
    { ...baseLayers, heights: true, temperature: true },
    { ...baseLayers, heights: true, biomes: true },
  ];
  const clamp = (value) => Math.max(0, Math.min(1, value));
  const ease = (value) => value * value * (3 - 2 * value);
  const wrap = (value) => ((value % 1) + 1) % 1;
  const hasField = (atlas, name) => Boolean(atlas && atlas[name]
    && atlas[name].length === atlas.width * atlas.height);

  function formation(stageIndex, progress) {
    if (stageIndex === 0) return { from: 0, to: 1, blend: ease(progress) };
    if (stageIndex === 1) return { from: 1, to: 2, blend: ease(progress) };
    if (stageIndex === 2 && progress < 0.45) {
      return { from: 2, to: 3, blend: ease(progress / 0.45) };
    }
    if (stageIndex === 2) return { from: 3, to: 4, blend: ease((progress - 0.45) / 0.55) };
    return { from: 4, to: 4, blend: 1 };
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
      : Math.max(0, Math.min(stages.length - 1, Math.floor(model.stageIndex || 0)));
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
    const step = formation(stageIndex, progress);
    const getRaster = dataReady && renderer && typeof renderer.getRaster === "function"
      ? renderer.getRaster : null;
    const from = getRaster ? getRaster(atlas, layerSets[step.from]) : null;
    const to = getRaster ? getRaster(atlas, layerSets[step.to]) : null;
    const mapReady = Boolean(from && to && from.width > 0 && from.height > 0
      && from.width === to.width && from.height === to.height);

    context.fillStyle = "#15272f";
    context.fillRect(0, 0, width, height);
    api.text("Creating your world", margin, margin, titleScale, colors.light);
    if (dataReady) api.text("Seed " + (atlas.seed === undefined ? "?" : atlas.seed),
      margin, margin + titleScale * 16 + 12, 1, colors.quiet);
    api.text(String(stageIndex + 1).padStart(2, "0") + " / 04", margin, bodyTop, 1, colors.quiet);
    api.text(stages[stageIndex].label, margin, bodyTop + 24, titleScale, colors.accent);
    const descriptionY = bodyTop + 28 + titleScale * 16;
    descriptions[stageIndex].forEach((line, index) => {
      api.text(line, margin, descriptionY + index * (bodyScale * 16 + 6), bodyScale, colors.light);
    });

    if (wide && height >= 560) {
      const listScale = 2;
      const listY = Math.max(descriptionY + bodyScale * 38 + 28,
        height - margin - 110 - stages.length * (listScale * 16 + 12));
      stages.forEach((stage, index) => {
        const prefix = index < stageIndex ? "+ " : index === stageIndex ? "> " : "  ";
        api.text(prefix + stage.label, margin, listY + index * (listScale * 16 + 12), listScale,
          index === stageIndex ? colors.accent : index < stageIndex ? colors.light : colors.quiet);
      });
    }

    if (mapReady) {
      const scale = Math.min(fieldWidth / from.width, Math.max(1, fieldBottom - fieldY) / from.height);
      const bounds = {
        width: Math.floor(from.width * scale), height: Math.floor(from.height * scale),
      };
      bounds.x = Math.round(fieldX + (fieldWidth - bounds.width) / 2);
      bounds.y = Math.round(fieldY + (fieldBottom - fieldY - bounds.height) / 2);
      context.save();
      context.imageSmoothingEnabled = false;
      context.globalAlpha = 1;
      // Arrival draws the shared default H+B canvas directly, without an intermediate raster.
      context.drawImage(from, bounds.x, bounds.y, bounds.width, bounds.height);
      if (from !== to && step.blend > 0) {
        context.globalAlpha = step.blend;
        context.drawImage(to, bounds.x, bounds.y, bounds.width, bounds.height);
      }
      context.restore();
      if (stageIndex === 3) drawMarker(api, bounds, model.player);
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
    } else if (mapReady && stageIndex === 1
      && !hasField(atlas, "moisture") && !hasField(atlas, "precipitation")) {
      status = "Moisture and precipitation unavailable";
    } else if (mapReady && stageIndex === 2 && !hasField(atlas, "climateZone")) {
      status = "Climate zones unavailable";
    }
    const footerScale = phase === "ready" && width >= 760 ? 2 : 1;
    api.text(status, margin, height - margin - footerScale * 32 - 8, footerScale, statusColor);
    api.text(action, margin, height - margin - footerScale * 16, footerScale, colors.accent);
    if (phase === "ready") api.text("Esc  Cancel", width - margin - 88,
      height - margin - 16, 1, colors.quiet);
  }

  window.S2GenerationView = { stages, durationMs: 16000, draw };
})();
