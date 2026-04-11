FROM python:3.12-slim

# Install g++ (Debian bookworm ships GCC 12 — full C++20: barrier, jthread, latch)
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Build the C++ binary
RUN mkdir -p build && \
    g++ -std=c++20 -O3 -pthread -Iinclude -o build/celeris src/main.cpp

# Install Python dependencies
RUN pip install --no-cache-dir -r web/requirements.txt

EXPOSE 10000
ENV PYTHONPATH=/app

CMD ["sh", "-c", "uvicorn web.app:app --host 0.0.0.0 --port ${PORT:-10000}"]
