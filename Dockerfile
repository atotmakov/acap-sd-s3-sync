ARG ARCH=aarch64
ARG SDK_VERSION=latest
FROM axisecp/acap-native-sdk:${SDK_VERSION}-${ARCH}

COPY app/ /opt/app/
WORKDIR /opt/app
RUN . /opt/axis/acapsdk/environment-setup* && acap-build .
