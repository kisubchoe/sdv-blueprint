FROM python:3.11-slim

WORKDIR /app

# Copy bridge code (build context: kuksa-bridge)
COPY bridge.py /app/bridge.py
COPY .env /app/.env

# Copy trigger scripts
COPY script/trigger-on.sh /app/script/trigger-on.sh
COPY script/trigger-off.sh /app/script/trigger-off.sh

# Install bash, podman, stress-ng and other dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    bash \
    podman \
    stress-ng \
    procps \
    psmisc \
    util-linux \
    tini \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies
RUN pip install --no-cache-dir pyserial kuksa-client python-dotenv

# Make scripts executable
RUN chmod +x /app/script/*.sh

# Disable Python output buffering for real-time logs
ENV PYTHONUNBUFFERED=1

ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["python3", "-u", "bridge.py"]
