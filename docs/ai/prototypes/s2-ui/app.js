(() => {
  "use strict";
  const canvas = document.getElementById("screen");
  const context = canvas.getContext("2d", { alpha: false });
  const font = window.S2_FONT_8X16;
  const atlas = window.S2_WORLD_DATA;
  const screens = ["loading", "generation", "game", "map"];
  const labels = { loading: "Loading", generation: "Generation", game: "Game", map: "World map" };
  const colors = { light: "#f3ead2", dark: "#10212b", accent: "#ffe080", quiet: "#a9babd", mapBackground: "#1d2d34" };
  const materials = ["Grass", "Stone"];
  const modes = [
    { name: "Creative", flight: true, bypass: false, canEdit: true, breakRule: "Any block - instant", placeRule: "Any block" },
    { name: "Survival", flight: false, bypass: false, canEdit: true, breakRule: "Normal timing; no Bedrock", placeRule: "Any block except Bedrock" },
    { name: "Spectator", flight: true, bypass: true, canEdit: false, breakRule: "Not allowed", placeRule: "Not allowed" },
  ];
  const views = ["FIRST", "REAR", "FRONT"];
  const speedProfiles = [2, 3, 5, 8, 15, 30, 80, 200, 500];
  const player = { x: 148 / 256, y: 112 / 256, heading: 0 };
  const generationStages = window.S2GenerationView.stages;
  const durationMs = window.S2GenerationView.durationMs;
  const stageDurationMs = durationMs / generationStages.length;
  const frameIntervalMs = 1000 / 24;
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
  let currentScreen = screens.includes(location.hash.slice(1)) ? location.hash.slice(1) : "loading";
  let loadingPhase = "preparing";
  let loadingCancelled = false;
  let generationPhase = "sample";
  let stageIndex = 0;
  let elapsedMs = 0;
  let lastTimestamp = 0;
  let frameRequest = 0;
  let timerRequest = 0;
  let selectedMaterial = 0;
  let gameMode = 0;
  let movementBits = 1;
  let speedIndex = 2;
  let acceleration = false;
  let viewIndex = 0;
  let hudVisible = true;
  const mapState = window.S2WorldMapView.createState();
  const terrainImage = new Image();
  terrainImage.addEventListener("load", draw);
  terrainImage.addEventListener("error", draw);
  terrainImage.src = "./assets/terrain.png";

  function imageIsReady(image) {
    return Boolean(image && image.complete && image.naturalWidth > 0 && image.naturalHeight > 0);
  }

  function datasetReady() {
    if (!atlas || !Number.isInteger(atlas.width) || !Number.isInteger(atlas.height)) return false;
    if (typeof window.S2WorldMapView.getRaster !== "function") return false;
    const cells = atlas.width * atlas.height;
    return cells > 0 && ["elevation", "waterLevel", "temperature", "climateZone", "oceanMask"].every(
      (name) => atlas[name] && atlas[name].length === cells,
    );
  }

  function sample(name) {
    if (!datasetReady() || !atlas[name]) return null;
    const index = Math.floor(player.y * atlas.height) * atlas.width + Math.floor(player.x * atlas.width);
    const value = Number(atlas[name][index]);
    return Number.isFinite(value) ? value : null;
  }

  function fontIsReady() {
    return Array.isArray(font) && font.length === 96;
  }

  function textScale(width, height) {
    return width >= 1000 && height >= 560 ? 2 : 1;
  }

  function drawGlyph(character, x, y, scale, color) {
    const code = character.charCodeAt(0);
    const glyphIndex = code >= 32 && code <= 127 ? code - 32 : "?".charCodeAt(0) - 32;
    const glyph = font[glyphIndex];
    const pixelX = Math.round(x);
    const pixelY = Math.round(y);
    if (!glyph) {
      return;
    }

    context.fillStyle = color;
    for (let row = 0; row < 16; row += 1) {
      const packedRows = glyph[Math.floor(row / 4)] >>> 0;
      const byte = (packedRows >>> (8 * (3 - (row % 4)))) & 0xff;
      for (let column = 0; column < 8; column += 1) {
        if (((byte >>> (7 - column)) & 1) !== 0) {
          context.fillRect(pixelX + column * scale, pixelY + row * scale, scale, scale);
        }
      }
    }
  }

  function drawText(value, x, y, scale, color = colors.light) {
    let cursor = x;
    for (const character of String(value)) {
      drawGlyph(character, cursor, y, scale, color);
      cursor += 8 * scale;
    }
  }

  function centerText(value, y, width, scale, color = colors.light) {
    const textWidth = String(value).length * 8 * scale;
    drawText(value, Math.max(0, (width - textWidth) / 2), y, scale, color);
  }

  function drawCapture(image, x, y, size, sourceX = 0, sourceY = 0, sourceSize = 256) {
    if (!imageIsReady(image)) {
      return false;
    }
    context.drawImage(
      image,
      sourceX,
      sourceY,
      sourceSize,
      sourceSize,
      x,
      y,
      size,
      size,
    );
    return true;
  }

  function drawCover(image, width, height) {
    if (!imageIsReady(image)) {
      return false;
    }
    const scale = Math.max(width / image.naturalWidth, height / image.naturalHeight);
    const drawWidth = image.naturalWidth * scale;
    const drawHeight = image.naturalHeight * scale;
    context.drawImage(
      image,
      (width - drawWidth) / 2,
      (height - drawHeight) / 2,
      drawWidth,
      drawHeight,
    );
    return true;
  }

  function movement() {
    return { flight: Boolean(movementBits & 1), bypass: Boolean(movementBits & 2) };
  }

  function hudModel(minimapRect) {
    const mode = modes[gameMode];
    return {
      visible: hudVisible, mode: mode.name, selectedMaterial: materials[selectedMaterial],
      canPlace: mode.canEdit, canBreak: mode.canEdit, breakRule: mode.breakRule, placeRule: mode.placeRule,
      movement: movement(), view: views[viewIndex], selectedSpeed: speedProfiles[speedIndex],
      effectiveSpeed: acceleration ? speedProfiles[speedIndex] : 1, acceleration, minimapRect,
      telemetry: {
        fps: 60, uptime: "02:14", chunks: 18, memoryMiB: 42,
        x: 37888, z: 84.1, y: 28672, yaw: (180 - player.heading + 360) % 360, pitch: 0, roll: 0,
        temperatureC: sample("temperature"), humidity: null,
      },
    };
  }

  function apiFor(width, height) {
    const scale = textScale(width, height);
    return {
      context, width, height, scale, lineHeight: 20 * scale, colors, atlas, terrainImage, mapImage: null,
      text: drawText, outlinedText: drawOutlinedText, centerText, fitText: drawFitText, drawCover, drawCapture,
    };
  }

  function drawOutlinedText(value, x, y, scale, color, outline = colors.dark) {
    const edge = Math.max(1, Math.floor(scale / 2));
    for (const [dx, dy] of [[-edge,0],[edge,0],[0,-edge],[0,edge],[-edge,-edge],[edge,-edge],[-edge,edge],[edge,edge]]) {
      drawText(value, x + dx, y + dy, scale, outline);
    }
    drawText(value, x, y, scale, color);
  }

  function drawFitText(value, x, y, scale, width, color = colors.light) {
    const fitted = Math.max(1, Math.min(scale, Math.floor(width / (String(value).length * 8))));
    drawText(value, x, y, fitted, color);
    return fitted;
  }

  function draw() {
    const bounds = canvas.getBoundingClientRect();
    const width = Math.max(1, Math.round(bounds.width));
    const height = Math.max(1, Math.round(bounds.height));
    const dpr = Math.max(1, Math.round(window.devicePixelRatio || 1));
    if (canvas.width !== width * dpr || canvas.height !== height * dpr) {
      canvas.width = width * dpr;
      canvas.height = height * dpr;
    }
    context.setTransform(dpr, 0, 0, dpr, 0, 0);
    context.imageSmoothingEnabled = false;
    const api = apiFor(width, height);
    if (!fontIsReady()) {
      context.fillStyle = colors.dark;
      context.fillRect(0, 0, width, height);
      canvas.setAttribute("aria-label", "S2 UI prototype; bitmap font unavailable");
      return;
    }
    let state = labels[currentScreen];
    if (currentScreen === "loading") {
      window.S2LoadingView.draw(api, { phase: loadingPhase, cancelled: loadingCancelled, address: "localhost" });
      state += `, ${loadingCancelled ? "cancelled" : loadingPhase}`;
    } else if (currentScreen === "generation") {
      window.S2GenerationView.draw(api, {
        phase: generationPhase, stageIndex, elapsedMs, reducedMotion: reducedMotion.matches, player,
        stageProgress: generationPhase === "ready" || reducedMotion.matches ? 1 : Math.max(0, Math.min(1, (elapsedMs - stageIndex * stageDurationMs) / stageDurationMs)),
      });
      state += `, ${generationPhase}, ${generationStages[stageIndex].label}`;
    } else if (currentScreen === "game") {
      context.fillStyle = "#4c718f";
      context.fillRect(0, 0, width, height);
      drawCover(terrainImage, width, height);
      const minimapRect = window.S2MinimapView.draw(api, { player });
      const model = hudModel(minimapRect);
      window.S2GameHud.draw(api, model);
      drawOutlinedText("+", width / 2 - 8, height / 2 - 16, 2, colors.light);
      const selection = model.canPlace ? `Selected block: ${model.selectedMaterial}; B Change block` : "Block placement unavailable";
      const rights = `${model.mode}; ${selection}; Break: ${model.breakRule}; Place: ${model.placeRule}`;
      state += `, HUD ${hudVisible ? "visible" : "hidden"}; ${rights}; Flight ${model.movement.flight ? "on" : "off"}; Bypass ${model.movement.bypass ? "on" : "off"}; View ${model.view}; Speed ${model.effectiveSpeed}x effective, ${model.selectedSpeed}x selected; Acceleration ${model.acceleration ? "on" : "off"}; Temperature ${model.telemetry.temperatureC === null ? "unavailable" : model.telemetry.temperatureC.toFixed(1) + " C"}; humidity unavailable`;
    } else {
      const displayed = window.S2WorldMapView.draw(api, mapState);
      state += `, zoom ${mapState.zoom.toFixed(2)}, pan ${mapState.panX.toFixed(2)},${mapState.panY.toFixed(2)}; `;
      state += Object.entries(displayed.layers).map(([name, status]) => `${name} ${status}`).join(", ");
    }
    canvas.setAttribute("aria-label", `S2 UI prototype: ${state}`);
  }

  function focusCanvas() {
    if (document.activeElement !== canvas) canvas.focus({ preventScroll: true });
  }

  function stopAnimation() {
    cancelAnimationFrame(frameRequest);
    clearTimeout(timerRequest);
    frameRequest = 0;
    timerRequest = 0;
    lastTimestamp = 0;
  }

  function startAnimation() {
    if (currentScreen !== "generation" || generationPhase !== "sample" || reducedMotion.matches
        || document.visibilityState === "hidden" || frameRequest || timerRequest) return;
    timerRequest = window.setTimeout(() => {
      timerRequest = 0;
      frameRequest = requestAnimationFrame(tick);
    }, frameIntervalMs);
  }

  function tick(timestamp) {
    frameRequest = 0;
    if (currentScreen !== "generation" || generationPhase !== "sample" || reducedMotion.matches
        || document.visibilityState === "hidden") return;
    if (lastTimestamp) elapsedMs += Math.min(150, timestamp - lastTimestamp);
    lastTimestamp = timestamp;
    if (elapsedMs >= durationMs) {
      elapsedMs = durationMs;
      stageIndex = generationStages.length - 1;
      generationPhase = datasetReady() ? "ready" : "error";
    } else stageIndex = Math.floor(elapsedMs / stageDurationMs);
    draw();
    startAnimation();
  }

  function resetGeneration() {
    stopAnimation();
    generationPhase = datasetReady() ? "sample" : "error";
    stageIndex = 0;
    elapsedMs = 0;
  }

  function setScreen(screen, updateHash = true) {
    if (!screens.includes(screen)) return;
    if (screen !== currentScreen) {
      stopAnimation();
      if (screen === "loading") { loadingPhase = "preparing"; loadingCancelled = false; }
      if (screen === "generation") resetGeneration();
    }
    currentScreen = screen;
    if (updateHash && location.hash !== `#${screen}`) location.hash = `#${screen}`;
    focusCanvas();
    draw();
    startAnimation();
  }

  function generationEnter() {
    if (generationPhase === "error") { location.reload(); return; }
    if (generationPhase === "cancelled") resetGeneration();
    else if (stageIndex < generationStages.length - 1) {
      stageIndex += 1;
      elapsedMs = stageIndex * stageDurationMs;
    } else {
      elapsedMs = durationMs;
      generationPhase = datasetReady() ? "ready" : "error";
      stopAnimation();
    }
    draw();
    startAnimation();
  }

  function cycleMode() {
    gameMode = (gameMode + 1) % modes.length;
    movementBits = (modes[gameMode].flight ? 1 : 0) | (modes[gameMode].bypass ? 2 : 0);
  }

  function cycleMovement() {
    const proposed = movementBits === 3 ? 1 : movementBits === 1 ? 0 : 3;
    const grant = (modes[gameMode].flight ? 1 : 0) | (modes[gameMode].bypass ? 2 : 0);
    movementBits = proposed & grant;
  }

  window.addEventListener("keydown", (event) => {
    const key = event.key.toLowerCase();
    if (event.target instanceof Element && event.target.closest("a")) return;
    if (event.metaKey || event.altKey || (event.ctrlKey && key !== "control")) return;
    const repeatable = currentScreen === "map" && ["arrowleft","arrowright","arrowup","arrowdown","+","=","-","_"].includes(key);
    if (event.repeat && !repeatable) return;
    if (currentScreen === "generation" && generationPhase === "ready") {
      if (key === "escape") { generationPhase = "cancelled"; stopAnimation(); draw(); }
      else setScreen("game");
      event.preventDefault();
      return;
    }
    if (/^[1-4]$/.test(key)) { setScreen(screens[Number(key) - 1]); event.preventDefault(); return; }
    if (currentScreen === "map") {
      if (key === "m" || key === "escape") setScreen("game");
      else window.S2WorldMapView.handleKey(mapState, event.key, event.shiftKey);
      event.preventDefault();
      draw();
      return;
    }
    let handled = true;
    if (currentScreen === "loading" && key === "enter") {
      if (loadingCancelled) { loadingCancelled = false; loadingPhase = "preparing"; }
      else if (loadingPhase === "preparing") loadingPhase = "connecting";
      else setScreen("generation");
    } else if (currentScreen === "loading" && key === "escape") loadingCancelled = true;
    else if (currentScreen === "generation" && key === "enter") generationEnter();
    else if (currentScreen === "generation" && key === "escape") { generationPhase = "cancelled"; stopAnimation(); }
    else if (currentScreen === "game") {
      if (key === "m") setScreen("map");
      else if (key === "b" && modes[gameMode].canEdit) selectedMaterial = (selectedMaterial + 1) % materials.length;
      else if (key === "g") cycleMode();
      else if (key === "f6" || key === "6") cycleMovement();
      else if (key === "f5" || key === "5") viewIndex = (viewIndex + 1) % views.length;
      else if (key === "f1") hudVisible = !hudVisible;
      else if (key === "+" || key === "=") speedIndex = (speedIndex + 1) % speedProfiles.length;
      else if (key === "-") speedIndex = (speedIndex + speedProfiles.length - 1) % speedProfiles.length;
      else if (key === "control") acceleration = !acceleration;
      else if (key === "escape") setScreen("loading");
      else handled = false;
    } else handled = false;
    if (handled) { event.preventDefault(); draw(); }
  });

  window.addEventListener("resize", draw);
  window.addEventListener("hashchange", () => setScreen(location.hash.slice(1), false));
  canvas.addEventListener("pointerdown", focusCanvas);
  document.querySelectorAll(".outside-frame a").forEach((link) => link.addEventListener("click", () => requestAnimationFrame(focusCanvas)));
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") stopAnimation();
    else startAnimation();
  });
  reducedMotion.addEventListener("change", () => { stopAnimation(); draw(); startAnimation(); });
  if (currentScreen === "generation") resetGeneration();
  draw();
  focusCanvas();
  startAnimation();
})();
