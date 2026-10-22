(() => {
  "use strict";

  const canvas = document.getElementById("screen");
  const context = canvas.getContext("2d", { alpha: false });
  const screens = ["loading", "generation", "game", "map"];
  const labels = {
    loading: "Loading",
    generation: "Generation",
    game: "Game",
    map: "World map",
  };
  const colors = {
    light: "#f3ead2",
    dark: "#10212b",
    accent: "#ffe080",
    quiet: "#87979a",
    mapBackground: "#1d2d34",
  };
  const font = window.S2_FONT_8X16;
  const materials = ["Grass", "Stone"];
  const modes = ["Creative", "Survival", "Spectator"];
  const views = ["FIRST", "REAR", "FRONT"];
  const speedupProfiles = [2, 3, 5, 8, 15, 30, 80, 200, 500];
  const mapMaterialPalette = {
    air: [189, 217, 237],
    stone: [128, 128, 128],
    grass: [92, 147, 69],
    dirt: [136, 103, 72],
    bedrock: [42, 40, 49],
    snow: [244, 247, 250],
    ice: [128, 201, 214],
  };
  const generationStages = [
    { label: "Land", caption: "Raising land relief" },
    { label: "Water", caption: "Forming water and drainage" },
    { label: "Climate", caption: "Applying climate bands" },
    { label: "Safe spawn", caption: "Checking a safe spawn" },
  ];
  const generationDurationMs = 14000;
  const generationStageDurationMs = generationDurationMs / generationStages.length;
  const generationFrameIntervalMs = 1000 / 18;
  const movementPresets = [
    { flight: true, bypass: false },
    { flight: false, bypass: false },
    { flight: true, bypass: true },
  ];
  const playerMapPosition = { x: 148, y: 112 };
  const localMapSourceSize = 64;
  const localMapSourceX = playerMapPosition.x - localMapSourceSize / 2;
  const localMapSourceY = playerMapPosition.y - localMapSourceSize / 2;

  const terrainImage = loadImage("./assets/terrain.png");
  let mapImageFailed = false;
  const mapImage = loadImage(
    window.S2_MAP_SOURCE || "./assets/maps.png",
    prepareMapRaster,
    () => {
      mapImageFailed = true;
      if (currentScreen === "generation") {
        generationPhase = "error";
        stopGenerationAnimation();
      }
    },
  );
  const sourceMapCanvas = document.createElement("canvas");
  sourceMapCanvas.width = 256;
  sourceMapCanvas.height = 256;
  const sourceMapContext = sourceMapCanvas.getContext("2d", { willReadFrequently: true });
  const localMapCanvas = document.createElement("canvas");
  localMapCanvas.width = localMapSourceSize;
  localMapCanvas.height = localMapSourceSize;
  const localMapContext = localMapCanvas.getContext("2d");
  const generationCanvas = document.createElement("canvas");
  generationCanvas.width = 256;
  generationCanvas.height = 256;
  const generationContext = generationCanvas.getContext("2d");
  let sourceMapPixels = null;
  let sourceMapLuma = null;
  let materialMapPixels = null;
  let localMapPixelsReady = false;
  const reducedMotionQuery = window.matchMedia
    ? window.matchMedia("(prefers-reduced-motion: reduce)")
    : { matches: false, addEventListener() {} };

  let currentScreen = screens.includes(location.hash.slice(1))
    ? location.hash.slice(1)
    : "loading";
  let loadingPhase = "preparing";
  let loadingCancelled = false;
  let generationPhase = "sample";
  let generationStageIndex = 0;
  let generationElapsedMs = 0;
  let generationLastTimestamp = 0;
  let generationFrameRequest = 0;
  let generationTimerRequest = 0;
  let generationLastPaint = 0;
  let selectedMaterial = 0;
  let gameMode = 0;
  let movementBits = 1;
  let selectedSpeedupIndex = 2;
  let accelerationEnabled = false;
  let viewIndex = 0;
  let debugHudVisible = true;
  let mapZoom = 1;
  let mapOffsetX = 0;
  let mapOffsetY = 0;

  function loadImage(source, onReady = () => {}, onError = () => {}) {
    const image = new Image();
    image.addEventListener("load", () => {
      onReady(image);
      draw();
    });
    image.addEventListener("error", () => {
      onError();
      draw();
    });
    image.src = source;
    return image;
  }

  function imageIsReady(image) {
    return image.complete && image.naturalWidth > 0 && image.naturalHeight > 0;
  }

  function materialForPixel(red, green, blue) {
    const brightness = (red * 0.2126) + (green * 0.7152) + (blue * 0.0722);
    if (brightness > 228) {
      return mapMaterialPalette.snow;
    }
    if (brightness < 48) {
      return mapMaterialPalette.bedrock;
    }
    const greenLead = green - Math.max(red, blue);
    const redLead = red - Math.max(green, blue);
    const blueLead = blue - Math.max(red, green);
    if (greenLead > 5) {
      return mapMaterialPalette.grass;
    }
    if (redLead > 7) {
      return mapMaterialPalette.dirt;
    }
    if (blueLead > 9) {
      return brightness < 142 ? mapMaterialPalette.ice : mapMaterialPalette.air;
    }
    return mapMaterialPalette.stone;
  }

  function prepareMapRaster(image) {
    if (!imageIsReady(image)) {
      return;
    }
    try {
      sourceMapContext.clearRect(0, 0, 256, 256);
      sourceMapContext.drawImage(image, 0, 0, 256, 256, 0, 0, 256, 256);
      sourceMapPixels = sourceMapContext.getImageData(0, 0, 256, 256).data;
      const materialImage = sourceMapContext.createImageData(256, 256);
      sourceMapLuma = new Float32Array(256 * 256);
      for (let index = 0; index < sourceMapLuma.length; index += 1) {
        const pixel = index * 4;
        sourceMapLuma[index] = sourceMapPixels[pixel] * 0.2126
          + sourceMapPixels[pixel + 1] * 0.7152
          + sourceMapPixels[pixel + 2] * 0.0722;
      }

      materialMapPixels = materialImage.data;
      for (let y = 0; y < 256; y += 1) {
        for (let x = 0; x < 256; x += 1) {
          const index = (y * 256) + x;
          const pixel = index * 4;
          const left = sourceMapLuma[(y * 256) + Math.max(0, x - 1)];
          const right = sourceMapLuma[(y * 256) + Math.min(255, x + 1)];
          const above = sourceMapLuma[(Math.max(0, y - 1) * 256) + x];
          const below = sourceMapLuma[(Math.min(255, y + 1) * 256) + x];
          const relief = Math.min(0.07, (Math.abs(right - left) + Math.abs(below - above)) / 900);
          const shade = 0.88 + (sourceMapLuma[index] / 255) * 0.16 + relief;
          const material = materialForPixel(
            sourceMapPixels[pixel],
            sourceMapPixels[pixel + 1],
            sourceMapPixels[pixel + 2],
          );
          materialMapPixels[pixel] = Math.min(255, material[0] * shade);
          materialMapPixels[pixel + 1] = Math.min(255, material[1] * shade);
          materialMapPixels[pixel + 2] = Math.min(255, material[2] * shade);
          materialMapPixels[pixel + 3] = 255;
        }
      }
      sourceMapContext.putImageData(materialImage, 0, 0);
      const localImage = localMapContext.createImageData(localMapSourceSize, localMapSourceSize);
      for (let y = 0; y < localMapSourceSize; y += 1) {
        for (let x = 0; x < localMapSourceSize; x += 1) {
          const sourceIndex = (((localMapSourceY + y) * 256) + localMapSourceX + x) * 4;
          const localIndex = ((y * localMapSourceSize) + x) * 4;
          localImage.data[localIndex] = materialMapPixels[sourceIndex];
          localImage.data[localIndex + 1] = materialMapPixels[sourceIndex + 1];
          localImage.data[localIndex + 2] = materialMapPixels[sourceIndex + 2];
          localImage.data[localIndex + 3] = 255;
        }
      }
      localMapContext.putImageData(localImage, 0, 0);
      localMapPixelsReady = true;
      if (generationPhase === "error" && !mapImageFailed) {
        generationPhase = "ready";
        generationStageIndex = generationStages.length - 1;
      }
    } catch (error) {
      sourceMapPixels = null;
      sourceMapLuma = null;
      materialMapPixels = null;
      localMapPixelsReady = false;
    }
  }

  function drawGenerationRaster(stageIndex, stageProgress, elapsedMs) {
    if (!sourceMapPixels || !sourceMapLuma || !materialMapPixels) {
      return false;
    }

    const image = generationContext.createImageData(256, 256);
    const output = image.data;
    const centerX = 128;
    const centerY = 128;
    const landRadius = 10 + stageProgress * 175;
    const flowOffset = elapsedMs * 0.016;
    const climateShift = elapsedMs * 0.00042;

    for (let y = 0; y < 256; y += 1) {
      const wave = landRadius + Math.sin((y * 0.07) + (elapsedMs * 0.001)) * 5;
      const climateBand = (Math.sin((y * 0.055) + climateShift) + 1) * 0.5;
      for (let x = 0; x < 256; x += 1) {
        const index = (y * 256) + x;
        const pixel = index * 4;
        let red = materialMapPixels[pixel];
        let green = materialMapPixels[pixel + 1];
        let blue = materialMapPixels[pixel + 2];

        if (stageIndex === 0) {
          const offsetX = x - centerX;
          const offsetY = y - centerY;
          const distanceSquared = (offsetX * offsetX) + (offsetY * offsetY);
          if (distanceSquared > wave * wave) {
            red *= 0.3;
            green *= 0.3;
            blue *= 0.3;
          } else if (distanceSquared > (wave - 3) * (wave - 3)) {
            red = 255;
            green = 224;
            blue = 128;
          }
        } else if (stageIndex === 1) {
          const reliefLine = Math.abs(((sourceMapLuma[index] + flowOffset) % 34) - 17);
          if (reliefLine < 2.4 && stageProgress > 0.02) {
            const blend = Math.min(0.8, stageProgress * 0.85);
            red = red * (1 - blend) + mapMaterialPalette.air[0] * blend;
            green = green * (1 - blend) + mapMaterialPalette.air[1] * blend;
            blue = blue * (1 - blend) + mapMaterialPalette.air[2] * blend;
          }
        } else if (stageIndex === 2) {
          const coldWeight = Math.max(0, Math.min(0.72, (climateBand - 0.48) * 1.5))
            * Math.max(0.2, Math.min(1, sourceMapLuma[index] / 190)) * stageProgress;
          red = red * (1 - coldWeight) + mapMaterialPalette.snow[0] * coldWeight;
          green = green * (1 - coldWeight) + mapMaterialPalette.snow[1] * coldWeight;
          blue = blue * (1 - coldWeight) + mapMaterialPalette.snow[2] * coldWeight;
        } else {
          red = sourceMapPixels[pixel];
          green = sourceMapPixels[pixel + 1];
          blue = sourceMapPixels[pixel + 2];
        }

        output[pixel] = red;
        output[pixel + 1] = green;
        output[pixel + 2] = blue;
        output[pixel + 3] = 255;
      }
    }

    if (stageIndex === 3) {
      const markerX = playerMapPosition.x;
      const markerY = playerMapPosition.y;
      for (let offset = -5; offset <= 5; offset += 1) {
        for (const [x, y] of [[markerX + offset, markerY], [markerX, markerY + offset]]) {
          const pixel = ((y * 256) + x) * 4;
          output[pixel] = 255;
          output[pixel + 1] = 224;
          output[pixel + 2] = 128;
          output[pixel + 3] = 255;
        }
      }
    }

    generationContext.putImageData(image, 0, 0);
    return true;
  }

  function fontIsReady() {
    return Array.isArray(font) && font.length === 96;
  }

  function textScale(width, height) {
    return Math.max(1, Math.min(2, Math.floor(Math.min(width / 640, height / 380))));
  }

  function titleScale(scale) {
    return Math.min(3, scale + 1);
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

  function drawLoading(width, height, scale, lineHeight) {
    context.fillStyle = "#4c718f";
    context.fillRect(0, 0, width, height);
    const terrainLoaded = drawCover(terrainImage, width, height);
    const titleSize = titleScale(scale);
    const bodyX = Math.max(24, Math.round(width * 0.08));
    const titleY = Math.round(height * 0.16);
    let lines;
    if (loadingCancelled) {
      lines = [
        loadingPhase === "connecting" ? "Connection cancelled" : "Preparation cancelled",
        "No connection is running",
        "[Enter] Retry | [1-4] Switch screens",
      ];
    } else if (loadingPhase === "preparing") {
      lines = [
        "Preparing to connect",
        "Phase: Loading client resources",
        "Address: localhost",
        "[Enter] Connect | [Esc] Cancel",
      ];
    } else {
      lines = [
        "Connecting to localhost",
        "Phase: Connecting",
        "[Enter] Continue fixture | [Esc] Cancel",
      ];
    }
    const textColor = colors.dark;
    drawText(terrainLoaded ? lines[0] : "Terrain capture missing", bodyX, titleY, titleSize, textColor);
    lines.slice(1).forEach((line, index) => {
      drawText(line, bodyX, titleY + titleSize * 16 + 16 + index * lineHeight, scale, textColor);
    });
  }

  function drawGeneration(width, height, scale, lineHeight) {
    context.fillStyle = "#202b2b";
    context.fillRect(0, 0, width, height);
    const compact = height < 520 || width < 900;
    const titleSize = titleScale(scale);
    const size = compact
      ? Math.min(height - 104, width * 0.42, 360)
      : Math.min(width * 0.7, height * 0.68, 640);
    const mapX = compact ? width - size - 24 : (width - size) / 2;
    const mapY = compact ? 56 : Math.max(titleSize * 16 + 48, (height - size) / 2 + 8);
    drawText("Creating your world", 24, 16, titleSize, colors.light);
    let stageX = 24;
    generationStages.forEach((stage, index) => {
      const color = index === generationStageIndex
        ? colors.accent
        : index < generationStageIndex ? colors.light : colors.quiet;
      drawText(stage.label, stageX, titleSize * 16 + 32, scale, color);
      if (index < generationStages.length - 1) {
        drawText(">", stageX + (stage.label.length + 1) * 8 * scale,
          titleSize * 16 + 32, scale, colors.quiet);
      }
      stageX += (stage.label.length + 4) * 8 * scale;
    });
    const stageProgress = reducedMotionQuery.matches
      ? 1
      : Math.max(0, Math.min(1,
        (generationElapsedMs - generationStageIndex * generationStageDurationMs)
          / generationStageDurationMs,
      ));
    const rasterReady = (generationPhase === "sample"
      || generationPhase === "ready"
      || generationPhase === "cancelled")
      && drawGenerationRaster(generationStageIndex,
        generationPhase === "ready" ? 1 : stageProgress, generationElapsedMs);
    if (rasterReady) {
      const previousSmoothing = context.imageSmoothingEnabled;
      context.imageSmoothingEnabled = false;
      context.drawImage(generationCanvas, mapX, mapY, size, size);
      context.imageSmoothingEnabled = previousSmoothing;
    }
    const detailX = compact ? 24 : mapX;
    const detailY = compact
      ? Math.max(titleSize * 16 + lineHeight * 2 + 24, height - lineHeight * 4 - 10)
      : mapY + size + 12;
    if (generationPhase === "cancelled") {
      drawText("Generation cancelled", detailX, detailY, scale, colors.light);
      drawText("[Enter] Retry | [1-4] Switch screens", detailX, detailY + lineHeight, scale, colors.accent);
    } else if (generationPhase === "error") {
      drawText("Map capture missing", detailX, detailY, titleSize, colors.accent);
      drawText("[Esc] Cancel | [1-4] Switch screens", detailX, detailY + titleSize * 16 + 4, scale, colors.light);
    } else if (generationPhase === "ready") {
      drawText("World ready", detailX, detailY, titleSize, colors.light);
      drawText("Press any key to start", detailX, detailY + titleSize * 16 + 4, scale, colors.accent);
    } else {
      drawText(generationStages[generationStageIndex].caption, detailX, detailY, scale, colors.light);
      drawText(
        reducedMotionQuery.matches
          ? "Manual sample | Reduced motion"
          : `Illustrative animation: ${(generationElapsedMs / 1000).toFixed(1)} s`,
        detailX,
        detailY + lineHeight,
        scale,
        colors.light,
      );
      drawText("[Enter] Advance sample | [Esc] Cancel", detailX, detailY + lineHeight * 2, scale, colors.accent);
    }
  }

  function activeMovement() {
    return { flight: (movementBits & 1) !== 0, bypass: (movementBits & 2) !== 0 };
  }

  function permissionLine() {
    const movement = activeMovement();
    const heading = `${modes[gameMode]} (fly ${movement.flight ? "on" : "off"}, phase ${movement.bypass ? "on" : "off"})`;
    if (modes[gameMode] === "Survival") {
      return [`${heading} | ${materials[selectedMaterial]} [B]`,
        "Break normal, Bedrock denied | Place: no Bedrock"];
    }
    if (modes[gameMode] === "Spectator") return [heading, "Break: none | Place: none"];
    return [`${heading} | ${materials[selectedMaterial]} [B]`, "Break: all, quick | Place: all"];
  }

  function drawFitText(value, x, y, requestedScale, availableWidth, color = colors.light) {
    const widthAtScale = String(value).length * 8 * requestedScale;
    const scale = widthAtScale <= availableWidth
      ? requestedScale
      : Math.max(1, Math.floor(availableWidth / (String(value).length * 8)));
    drawText(value, x, y, scale, color);
    return scale;
  }

  function drawOutlinedText(value, x, y, scale, color, outline = colors.dark) {
    const edge = Math.max(1, Math.floor(scale / 2));
    for (const [offsetX, offsetY] of [
      [-edge, 0], [edge, 0], [0, -edge], [0, edge],
      [-edge, -edge], [edge, -edge], [-edge, edge], [edge, edge],
    ]) {
      drawText(value, x + offsetX, y + offsetY, scale, outline);
    }
    drawText(value, x, y, scale, color);
  }

  function drawMapCardinals(left, top, size, scale) {
    const cardinalScale = 2;
    const centerX = left + size / 2;
    const centerY = top + size / 2;
    drawOutlinedText("N", centerX - 4 * cardinalScale, top - 20 * cardinalScale,
      cardinalScale, colors.light);
    drawOutlinedText("W", left - 12 * cardinalScale, centerY - 8 * cardinalScale,
      cardinalScale, colors.light);
    drawOutlinedText("E", left + size + 4 * cardinalScale, centerY - 8 * cardinalScale,
      cardinalScale, colors.light);
    drawOutlinedText("S", centerX - 4 * cardinalScale, top + size + 3 * cardinalScale,
      cardinalScale, colors.light);
    return { centerX, centerY };
  }

  function drawGame(width, height, scale, lineHeight) {
    context.fillStyle = "#4c718f";
    context.fillRect(0, 0, width, height);
    const terrainLoaded = drawCover(terrainImage, width, height);
    if (!terrainLoaded) {
      drawText("Terrain capture missing", 8, height / 2, scale, colors.accent);
    }

    const mapSize = Math.max(72, Math.min(height * 0.34, width * 0.18, 220));
    const mapX = width - mapSize - 28;
    const mapY = 48;
    if (localMapPixelsReady) {
      const previousSmoothing = context.imageSmoothingEnabled;
      context.imageSmoothingEnabled = false;
      context.drawImage(localMapCanvas, mapX, mapY, mapSize, mapSize);
      context.imageSmoothingEnabled = previousSmoothing;
      context.strokeStyle = colors.dark;
      context.lineWidth = 1;
      context.strokeRect(mapX - 1, mapY - 1, mapSize + 2, mapSize + 2);
      const markerX = mapX + mapSize / 2;
      const markerY = mapY + mapSize / 2;
      drawOutlinedText("^", markerX - 4 * scale, markerY - 8 * scale, scale, colors.accent);
      drawOutlinedText("Local map", mapX + 4, mapY + 4, scale, colors.light);
      drawMapCardinals(mapX, mapY, mapSize, scale);
    } else {
      drawOutlinedText(mapImageFailed ? "Map capture missing" : "Local map loading",
        mapX, mapY, scale, colors.light);
    }

    if (debugHudVisible) {
      const hudLines = [
        "FPS: 60 | Uptime: 02:14 | Chunks: 18 | RSS: 42 MiB | F1 hide",
        "XYZ: 12.4, 84.1, 7.5",
        `YPR: 90.0, 0.0, 0.0 deg | View: ${views[viewIndex]} (F5/5)`,
        `Speed: ${accelerationEnabled ? speedupProfiles[selectedSpeedupIndex] : 1}x/${speedupProfiles[selectedSpeedupIndex]}x (+/-) | Accel ${accelerationEnabled ? "on" : "off"} (Ctrl) | Fly ${activeMovement().flight ? "on" : "off"} | Bypass ${activeMovement().bypass ? "on" : "off"} (F6/6)`,
        "Temperature: 8.4 C | Humidity: --",
      ];
      const hudMaxWidth = Math.max(80, mapX - 20);
      hudLines.forEach((line, index) => {
        drawFitText(line, 8, 8 + index * (16 * scale + 3), scale, hudMaxWidth, colors.dark);
      });
    }

    drawOutlinedText("+", width / 2 - 4 * scale, height / 2 - 8 * scale, scale, colors.light);
    const footerY = height - lineHeight * 2 - 2;
    const rights = permissionLine();
    drawOutlinedText(rights[0], 8, footerY, Math.min(2, scale), colors.light);
    const hint = debugHudVisible ? "G mode / F6 caps | M map | Esc" : "F1 HUD | G mode | M map";
    drawFitText(`${rights[1]} | ${hint}`, 8, footerY + lineHeight,
      scale, width - 16, colors.light);
    if (!debugHudVisible) {
      drawOutlinedText("F1 HUD", width - 6 * 8 * scale - 8, 8, scale, colors.light);
    }
  }

  function wrap(value, range) {
    return ((value % range) + range) % range;
  }

  function wrapCentered(value, range) {
    return wrap(value + range / 2, range) - range / 2;
  }

  function drawMap(width, height, scale, lineHeight) {
    context.fillStyle = colors.mapBackground;
    context.fillRect(0, 0, width, height);

    const size = Math.max(1, Math.min(width - 80, height - 124, 640));
    const left = (width - size) / 2;
    const top = (height - size) / 2;
    const mapLoaded = imageIsReady(mapImage);

    if (mapLoaded) {
      const drawnSize = size * mapZoom;
      mapOffsetX = wrapCentered(mapOffsetX, drawnSize);
      mapOffsetY = wrapCentered(mapOffsetY, drawnSize);
      context.save();
      context.beginPath();
      context.rect(left, top, size, size);
      context.clip();
      context.imageSmoothingEnabled = false;

      const shiftedX = wrap(mapOffsetX, drawnSize);
      const shiftedY = wrap(mapOffsetY, drawnSize);
      for (let row = -1; row <= 1; row += 1) {
        for (let column = -1; column <= 1; column += 1) {
          const tileX = left + size / 2 + shiftedX + column * drawnSize - drawnSize / 2;
          const tileY = top + size / 2 + shiftedY + row * drawnSize - drawnSize / 2;
          drawCapture(mapImage, tileX, tileY, drawnSize, 0, 0, 256);

          const markerX = tileX + (playerMapPosition.x / 256) * drawnSize;
          const markerY = tileY + (playerMapPosition.y / 256) * drawnSize;
          drawOutlinedText("+", markerX - 4 * scale, markerY - 8 * scale,
            scale, colors.accent);
        }
      }
      context.restore();
    } else {
      centerText("Map capture missing", top + size / 2, width, scale, colors.accent);
    }

    drawFitText("World map | + You", 8, 8,
      titleScale(scale), width - 16, colors.light);
    const zoomLabel = `Zoom ${mapZoom.toFixed(2)}x`;
    drawText(zoomLabel, width - zoomLabel.length * 8 * scale - 8, 8, scale, colors.light);
    drawMapCardinals(left, top, size, scale);
    drawFitText("Arrows/WASD pan | +/- zoom | R center | M/Esc back",
      8, height - lineHeight - 2, scale, width - 16, colors.light);
  }

  function draw() {
    const bounds = canvas.getBoundingClientRect();
    const width = Math.max(1, bounds.width);
    const height = Math.max(1, bounds.height);
    const deviceScale = Math.max(1, Math.round(window.devicePixelRatio || 1));

    const pixelWidth = Math.round(width * deviceScale);
    const pixelHeight = Math.round(height * deviceScale);
    if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
      canvas.width = pixelWidth;
      canvas.height = pixelHeight;
    }
    context.setTransform(deviceScale, 0, 0, deviceScale, 0, 0);
    context.imageSmoothingEnabled = true;

    const scale = textScale(width, height);
    const lineHeight = 20 * scale;
    if (!fontIsReady()) {
      context.fillStyle = "#202b2b";
      context.fillRect(0, 0, width, height);
      canvas.setAttribute("aria-label", "S2 UI prototype; bitmap font asset is unavailable");
      return;
    }

    if (currentScreen === "loading") {
      drawLoading(width, height, scale, lineHeight);
    } else if (currentScreen === "generation") {
      drawGeneration(width, height, scale, lineHeight);
    } else if (currentScreen === "game") {
      drawGame(width, height, scale, lineHeight);
    } else {
      drawMap(width, height, scale, lineHeight);
    }
    let state = labels[currentScreen];
    if (currentScreen === "loading") state += `, ${loadingCancelled ? "cancelled" : loadingPhase}`;
    if (currentScreen === "generation") state += `, ${generationPhase}, ${generationStages[generationStageIndex].label}`;
    if (currentScreen === "game") {
      state += `, ${debugHudVisible ? "debug HUD visible" : "debug HUD hidden"}; ${permissionLine().join("; ")}; humidity unavailable`;
    }
    if (currentScreen === "map") state += `, zoom ${mapZoom.toFixed(2)}, pan ${Math.round(mapOffsetX)},${Math.round(mapOffsetY)}`;
    canvas.setAttribute("aria-label", `S2 UI prototype: ${state}`);
  }

  function setScreen(screen, updateHash = true) {
    if (!screens.includes(screen)) {
      return;
    }
    if (screen !== currentScreen) {
      stopGenerationAnimation();
      if (screen === "loading") {
        loadingPhase = "preparing";
        loadingCancelled = false;
      } else if (screen === "generation") {
        generationPhase = mapImageFailed ? "error" : "sample";
        generationStageIndex = 0;
        generationElapsedMs = 0;
        generationLastPaint = 0;
      }
    }
    currentScreen = screen;
    const hash = `#${screen}`;
    if (updateHash && location.hash !== hash) {
      location.hash = hash;
    }
    focusCanvas();
    draw();
    if (currentScreen === "generation") {
      startGenerationAnimation();
    }
  }

  function focusCanvas() {
    if (document.activeElement !== canvas) {
      canvas.focus({ preventScroll: true });
    }
  }

  function stopGenerationAnimation() {
    if (generationFrameRequest !== 0) {
      cancelAnimationFrame(generationFrameRequest);
    }
    if (generationTimerRequest !== 0) {
      clearTimeout(generationTimerRequest);
    }
    generationFrameRequest = 0;
    generationTimerRequest = 0;
    generationLastTimestamp = 0;
  }

  function generationCanAnimate() {
    return currentScreen === "generation"
      && generationPhase === "sample"
      && !reducedMotionQuery.matches
      && document.visibilityState !== "hidden"
      && !mapImageFailed;
  }

  function startGenerationAnimation() {
    if (!generationCanAnimate() || generationFrameRequest !== 0 || generationTimerRequest !== 0) {
      return;
    }
    generationLastTimestamp = 0;
    generationTimerRequest = window.setTimeout(() => {
      generationTimerRequest = 0;
      generationFrameRequest = requestAnimationFrame(updateGenerationAnimation);
    }, generationFrameIntervalMs);
  }

  function updateGenerationAnimation(timestamp) {
    generationFrameRequest = 0;
    if (!generationCanAnimate()) {
      return;
    }
    if (generationLastTimestamp !== 0) {
      generationElapsedMs += Math.min(100, timestamp - generationLastTimestamp);
    }
    generationLastTimestamp = timestamp;
    if (generationElapsedMs >= generationDurationMs) {
      generationElapsedMs = generationDurationMs;
      generationStageIndex = generationStages.length - 1;
      generationPhase = localMapPixelsReady ? "ready" : "error";
      draw();
      return;
    }
    generationStageIndex = Math.min(
      generationStages.length - 1,
      Math.floor(generationElapsedMs / generationStageDurationMs),
    );
    if (timestamp - generationLastPaint >= generationFrameIntervalMs) {
      generationLastPaint = timestamp;
      draw();
    }
    generationTimerRequest = window.setTimeout(() => {
      generationTimerRequest = 0;
      generationFrameRequest = requestAnimationFrame(updateGenerationAnimation);
    }, generationFrameIntervalMs);
  }

  function startGenerationSample() {
    setScreen("generation");
    generationPhase = mapImageFailed ? "error" : "sample";
    generationStageIndex = 0;
    generationElapsedMs = 0;
    generationLastPaint = 0;
    if (generationPhase === "error") {
      stopGenerationAnimation();
    } else {
      startGenerationAnimation();
    }
    draw();
  }

  function handleLoadingEnter() {
    if (loadingCancelled) {
      loadingCancelled = false;
      loadingPhase = "preparing";
    } else if (loadingPhase === "preparing") {
      loadingPhase = "connecting";
    } else {
      startGenerationSample();
      return;
    }
    draw();
  }

  function handleGenerationEnter() {
    if (generationPhase === "cancelled") {
      startGenerationSample();
      return;
    } else if (generationPhase === "sample" && generationStageIndex < generationStages.length - 1) {
      generationStageIndex += 1;
      generationElapsedMs = generationStageIndex * generationStageDurationMs;
    } else if (generationPhase === "sample") {
      generationStageIndex = generationStages.length - 1;
      generationElapsedMs = generationDurationMs;
      generationPhase = localMapPixelsReady ? "ready" : "error";
    } else if (generationPhase === "ready") {
      setScreen("game");
      return;
    } else {
      return;
    }
    if (generationPhase === "sample") {
      startGenerationAnimation();
    } else {
      stopGenerationAnimation();
    }
    draw();
  }

  window.addEventListener("resize", draw);
  window.addEventListener("hashchange", () => {
    const screen = location.hash.slice(1);
    if (screens.includes(screen)) {
      setScreen(screen, false);
    }
  });
  canvas.addEventListener("pointerdown", focusCanvas);
  document.querySelectorAll(".outside-frame a").forEach((link) => {
    link.addEventListener("click", () => requestAnimationFrame(focusCanvas));
  });
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") {
      stopGenerationAnimation();
    } else {
      startGenerationAnimation();
    }
  });
  if (typeof reducedMotionQuery.addEventListener === "function") {
    reducedMotionQuery.addEventListener("change", () => {
      stopGenerationAnimation();
      draw();
      startGenerationAnimation();
    });
  }

  function advanceMode() {
    gameMode = (gameMode + 1) % modes.length;
    movementBits = movementPresets[gameMode].flight ? 1 : 0;
    if (movementPresets[gameMode].bypass) movementBits |= 2;
  }

  function cycleMovement() {
    const proposed = movementBits === 3 ? 1 : movementBits === 1 ? 0 : 3;
    const grant = movementPresets[gameMode];
    movementBits = proposed & ((grant.flight ? 1 : 0) | (grant.bypass ? 2 : 0));
  }

  window.addEventListener("keydown", (event) => {
    const key = event.key.toLowerCase();
    if (event.target instanceof Element && event.target.closest("a")) {
      return;
    }
    if (event.repeat && currentScreen !== "map") return;

    if (currentScreen === "generation" && generationPhase === "ready") {
      if (key === "escape") {
        generationPhase = "cancelled";
        stopGenerationAnimation();
        draw();
      } else {
        setScreen("game");
      }
      event.preventDefault();
      return;
    }

    if (/^[1-4]$/.test(key)) {
      setScreen(screens[Number(key) - 1]);
      return;
    }

    if (currentScreen === "map") {
      const panStep = event.shiftKey ? 48 : 24;
      if (key === "arrowleft" || key === "a") {
        mapOffsetX -= panStep;
      } else if (key === "arrowright" || key === "d") {
        mapOffsetX += panStep;
      } else if (key === "arrowup" || key === "w") {
        mapOffsetY -= panStep;
      } else if (key === "arrowdown" || key === "s") {
        mapOffsetY += panStep;
      } else if (key === "+" || key === "=") {
        mapZoom = Math.min(3, Number((mapZoom + 0.25).toFixed(2)));
      } else if (key === "-") {
        mapZoom = Math.max(0.5, Number((mapZoom - 0.25).toFixed(2)));
      } else if (key === "r") {
        mapZoom = 1;
        mapOffsetX = 0;
        mapOffsetY = 0;
      } else if (key === "m" || key === "escape") {
        setScreen("game");
        event.preventDefault();
        return;
      }
      if (["arrowleft", "arrowright", "arrowup", "arrowdown", "a", "d", "w", "s", "+", "=", "-", "r"].includes(key)) {
        event.preventDefault();
        draw();
      }
      return;
    }

    if (currentScreen === "loading" && key === "enter") {
      handleLoadingEnter();
      event.preventDefault();
    } else if (currentScreen === "loading" && key === "escape") {
      loadingCancelled = true;
      event.preventDefault();
      draw();
    } else if (currentScreen === "generation" && key === "enter") {
      handleGenerationEnter();
      event.preventDefault();
    } else if (currentScreen === "generation" && key === "escape") {
      generationPhase = "cancelled";
      stopGenerationAnimation();
      event.preventDefault();
      draw();
    } else if (key === "m" && currentScreen === "game") {
      setScreen("map");
      event.preventDefault();
    } else if (key === "b" && currentScreen === "game" && modes[gameMode] !== "Spectator") {
      selectedMaterial = (selectedMaterial + 1) % materials.length;
      draw();
    } else if (key === "g" && currentScreen === "game") {
      advanceMode();
      draw();
    } else if ((key === "f6" || key === "6") && currentScreen === "game") {
      cycleMovement();
      draw();
      event.preventDefault();
    } else if ((key === "f5" || key === "5") && currentScreen === "game") {
      viewIndex = (viewIndex + 1) % views.length;
      draw();
      event.preventDefault();
    } else if (key === "f1" && currentScreen === "game") {
      debugHudVisible = !debugHudVisible;
      draw();
      event.preventDefault();
    } else if ((key === "+" || key === "=") && currentScreen === "game") {
      selectedSpeedupIndex = (selectedSpeedupIndex + 1) % speedupProfiles.length;
      draw();
      event.preventDefault();
    } else if (key === "-" && currentScreen === "game") {
      selectedSpeedupIndex = (selectedSpeedupIndex + speedupProfiles.length - 1) % speedupProfiles.length;
      draw();
      event.preventDefault();
    } else if (key === "control" && currentScreen === "game") {
      accelerationEnabled = !accelerationEnabled;
      draw();
    } else if (key === "escape" && currentScreen === "game") {
      setScreen("loading");
      event.preventDefault();
    }
  });

  draw();
  focusCanvas();
  if (currentScreen === "generation") {
    startGenerationAnimation();
  }
})();
