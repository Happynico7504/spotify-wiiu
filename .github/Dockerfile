FROM devkitpro/devkitppc:latest

RUN dkp-pacman -Syu --noconfirm wiiu-dev \
    && dkp-pacman -Sc --noconfirm

# Native Rust toolchain + tools needed for setup-tool / bundle Linux builds.
# Install into /usr/local so the tools are available regardless of HOME,
# which GitHub Actions overrides inside container jobs.
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:${PATH}

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       curl musl-tools libasound2-dev libssl-dev pkg-config \
    && apt-get clean && rm -rf /var/lib/apt/lists/* \
    && curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
       | sh -s -- -y --default-toolchain stable --profile minimal \
          --target x86_64-unknown-linux-musl \
    && chmod -R a+w /usr/local/rustup /usr/local/cargo \
    && rm -rf /usr/local/rustup/toolchains/stable-x86_64-unknown-linux-gnu/share/doc
