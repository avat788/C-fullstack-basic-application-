FROM gcc:latest

# SQLite3 development headers (sqlite3.h) and library, needed to link -lsqlite3
RUN apt-get update && \
    apt-get install -y --no-install-recommends libsqlite3-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY main.c Makefile index.html ./

RUN make

EXPOSE 8080

CMD ["./app"]
