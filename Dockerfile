# NaturalAir De-Esser: build in Railway (Linux VST3) and serve the result for download.
# 1) Builds the plugin + runs the DSP regression tests (build fails if they fail).
# 2) Packages the .vst3 into a zip and serves it over HTTP on $PORT.
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates pkg-config zip \
      libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev \
      libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev \
      libgl1-mesa-dev mesa-common-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# -j2 keeps memory use modest on small Railway builders; raise if your plan allows.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNA_BUILD_TESTS=ON \
 && cmake --build build -j2 \
 && ctest --test-dir build --output-on-failure \
 && mkdir -p /out && cd build/NaturalAirDeEsser_artefacts/Release \
 && zip -r /out/NaturalAir-DeEsser-linux-VST3.zip VST3 \
 && if [ -d Standalone ]; then zip -r /out/NaturalAir-DeEsser-linux-Standalone.zip Standalone; fi

FROM python:3.12-slim
WORKDIR /out
COPY --from=build /out/ /out/
EXPOSE 8080
CMD ["sh", "-c", "python3 -m http.server ${PORT:-8080} --directory /out"]
