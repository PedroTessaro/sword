FROM debian:stable AS base

WORKDIR /usr/src/sword
COPY . .


RUN apt update
RUN apt install -y clang build-essential
#RUN apt install -y gcc make clang

FROM base AS sword

RUN make 
RUN make install

ENV PATH=$PATH:/root/.local/bin

CMD ["shield", "--help"]
# docker run -v $(pwd):/usr/src/test -it sword-docker bash