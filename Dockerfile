# Build stage: static binary so the final image needs no libc.
FROM gcc:14 AS build
WORKDIR /src
COPY . .
RUN make BUILDDIR=/out LDFLAGS=-static && strip /out/tspdf

# Final stage: just the binary. The web UI assets are embedded in it.
FROM scratch
COPY --from=build /out/tspdf /tspdf
EXPOSE 8080
ENTRYPOINT ["/tspdf", "serve", "--bind", "0.0.0.0", "--port", "8080"]
