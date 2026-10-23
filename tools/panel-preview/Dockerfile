FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build g++ pkg-config ca-certificates \
    libcairo2-dev libfreetype-dev libvulkan-dev libxcb1-dev fonts-noto-core fonts-noto-cjk fontconfig \
    && (apt-get install -y --no-install-recommends libopenvr-dev || true) && rm -rf /var/lib/apt/lists/*
