#include "local_device.h"
#include <aditof/frame_operations.h>

extern "C" {
#include "eeprom.h"
#include "temp_sensor.h"
}

#include <algorithm>
#include <fcntl.h>
#include <glog/logging.h>
#include <linux/videodev2.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

#define V4L2_CID_AD_DEV_SET_CHIP_CONFIG 0xA00A00
#define V4L2_CID_AD_DEV_READ_REG 0xA00A01
#define CTRL_PACKET_SIZE 4096

#define EEPROM_DEV_PATH "/sys/bus/i2c/devices/0-0056/eeprom"

#define TEMP_SENSOR_DEV_PATH "/dev/i2c-1"
#define LASER_TEMP_SENSOR_I2C_ADDR 0x49
#define AFE_TEMP_SENSOR_I2C_ADDR 0x4b

struct buffer {
    void *start;
    size_t length;
};

struct LocalDevice::ImplData {
    int fd;
    int sfd;
    struct buffer *videoBuffers;
    unsigned int nVideoBuffers;
    struct v4l2_plane planes[8];
    aditof::FrameDetails frameDetails;
    bool started;
};

// TO DO: This exists in linux_utils.h which is not included on Dragoboard.
// Should not have duplicated code if possible.
static int xioctl(int fh, unsigned int request, void *arg) {
    int r;

    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno && errno != 0);

    return r;
}

LocalDevice::LocalDevice(const aditof::DeviceConstructionData &data)
    : m_devData(data), m_implData(new LocalDevice::ImplData) {
    CLEAR(*m_implData);
}

LocalDevice::~LocalDevice() {
    if (m_implData->started) {
        stop();
    }

    for (unsigned int i = 0; i < m_implData->nVideoBuffers; i++) {
        if (munmap(m_implData->videoBuffers[i].start,
                   m_implData->videoBuffers[i].length) == -1) {
            LOG(WARNING) << "munmap error "
                         << "errno: " << errno << " error: " << strerror(errno);
        }
    }
    free(m_implData->videoBuffers);

    if (close(m_implData->fd) == -1) {
        LOG(WARNING) << "close m_implData->fd error "
                     << "errno: " << errno << " error: " << strerror(errno);
    }

    if (close(m_implData->sfd) == -1) {
        LOG(WARNING) << "close m_implData->sfd error "
                     << "errno: " << errno << " error: " << strerror(errno);
    }
}

aditof::Status LocalDevice::open() {
    using namespace aditof;
    Status status = Status::OK;

    struct stat st;
    struct v4l2_capability cap;

    std::vector<std::string> paths;
    std::stringstream ss(m_devData.driverPath);
    std::string token;
    while (std::getline(ss, token, ';')) {
        paths.push_back(token);
    }

    const char *devName = paths.front().c_str();
    const char *subDevName = paths.back().c_str();

    /* Open V4L2 device */
    if (stat(devName, &st) == -1) {
        LOG(WARNING) << "Cannot identify " << devName << "errno: " << errno
                     << "error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    if (!S_ISCHR(st.st_mode)) {
        LOG(WARNING) << devName << " is not a valid device";
        return Status::GENERIC_ERROR;
    }

    m_implData->fd = ::open(devName, O_RDWR | O_NONBLOCK, 0);
    if (m_implData->fd == -1) {
        LOG(WARNING) << "Cannot open " << devName << "errno: " << errno
                     << "error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    if (xioctl(m_implData->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        LOG(WARNING) << devName << " VIDIOC_QUERYCAP error";
        return Status::GENERIC_ERROR;
    }

    if (strcmp((char *)cap.card, "Qualcomm Camera Subsystem")) {
        LOG(WARNING) << "CAPTURE Device " << cap.card;
        return Status::GENERIC_ERROR;
    }

    if (!(cap.capabilities &
          (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
        LOG(WARNING) << devName << " is not a video capture device";
        return Status::GENERIC_ERROR;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG(WARNING) << devName << " does not support streaming i/o";
        return Status::GENERIC_ERROR;
    }

    /* Open V4L2 subdevice */
    if (stat(subDevName, &st) == -1) {
        LOG(WARNING) << "Cannot identify " << subDevName << " errno: " << errno
                     << " error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    if (!S_ISCHR(st.st_mode)) {
        LOG(WARNING) << subDevName << " is not a valid device";
        return Status::GENERIC_ERROR;
    }

    m_implData->sfd = ::open(subDevName, O_RDWR | O_NONBLOCK, 0);
    if (m_implData->sfd == -1) {
        LOG(WARNING) << "Cannot open " << subDevName << " errno: " << errno
                     << " error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    return status;
}

aditof::Status LocalDevice::start() {
    using namespace aditof;
    Status status = Status::OK;

    if (m_implData->started) {
        LOG(INFO) << "Device already started";
        return Status::BUSY;
    }
    LOG(INFO) << "Starting device";

    struct v4l2_buffer buf;
    for (unsigned int i = 0; i < m_implData->nVideoBuffers; i++) {
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = m_implData->planes;
        buf.length = 1;

        if (xioctl(m_implData->fd, VIDIOC_QBUF, &buf) == -1) {
            LOG(WARNING) << "mmap error "
                         << "errno: " << errno << " error: " << strerror(errno);
            return Status::GENERIC_ERROR;
        }
    }

    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(m_implData->fd, VIDIOC_STREAMON, &type) == -1) {
        LOG(WARNING) << "VIDIOC_STREAMON error "
                     << "errno: " << errno << " error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    m_implData->started = true;

    return status;
}

aditof::Status LocalDevice::stop() {
    using namespace aditof;
    Status status = Status::OK;

    if (!m_implData->started) {
        LOG(INFO) << "Device already stopped";
        return Status::BUSY;
    }
    LOG(INFO) << "Stopping device";

    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(m_implData->fd, VIDIOC_STREAMOFF, &type) == -1) {
        LOG(WARNING) << "VIDIOC_STREAMOFF error "
                     << "errno: " << errno << " error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    m_implData->started = false;

    return status;
}

aditof::Status
LocalDevice::getAvailableFrameTypes(std::vector<aditof::FrameDetails> &types) {
    using namespace aditof;
    Status status = Status::OK;

    FrameDetails details;

    details.width = 640;
    details.height = 960;
    details.cal_data.offset = 0;
    details.cal_data.gain = 1;
    details.type = "depth_ir";
    types.push_back(details);

    details.width = 668;
    details.height = 750;
    details.cal_data.offset = 0;
    details.cal_data.gain = 1;
    details.type = "raw";
    types.push_back(details);

    return status;
}

aditof::Status LocalDevice::setFrameType(const aditof::FrameDetails &details) {
    using namespace aditof;
    Status status = Status::OK;

    struct v4l2_requestbuffers req;
    struct v4l2_format fmt;
    struct v4l2_buffer buf;

    if (details != m_implData->frameDetails) {
        for (unsigned int i = 0; i < m_implData->nVideoBuffers; i++) {
            if (munmap(m_implData->videoBuffers[i].start,
                       m_implData->videoBuffers[i].length) == -1) {
                LOG(WARNING)
                    << "munmap error "
                    << "errno: " << errno << " error: " << strerror(errno);
                return Status::GENERIC_ERROR;
            }
        }
        free(m_implData->videoBuffers);
        m_implData->nVideoBuffers = 0;
    } else if (m_implData->nVideoBuffers) {
        return status;
    }

    /* Set the frame format in the driver */
    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix.width = details.width;
    fmt.fmt.pix.height = details.height;

    if (xioctl(m_implData->fd, VIDIOC_S_FMT, &fmt) == -1) {
        LOG(WARNING) << "Setting Pixel Format error, errno: " << errno
                     << " error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    /* Allocate the video buffers in the driver */
    CLEAR(req);
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(m_implData->fd, VIDIOC_REQBUFS, &req) == -1) {
        LOG(WARNING) << "VIDIOC_REQBUFS error "
                     << "errno: " << errno << " error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    m_implData->videoBuffers =
        (buffer *)calloc(req.count, sizeof(*m_implData->videoBuffers));
    if (!m_implData->videoBuffers) {
        LOG(WARNING) << "Failed to allocate video m_implData->videoBuffers";
        return Status::GENERIC_ERROR;
    }

    for (m_implData->nVideoBuffers = 0; m_implData->nVideoBuffers < req.count;
         m_implData->nVideoBuffers++) {
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = m_implData->nVideoBuffers;
        buf.m.planes = m_implData->planes;
        buf.length = 1;

        if (xioctl(m_implData->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            LOG(WARNING) << "VIDIOC_QUERYBUF error "
                         << "errno: " << errno << " error: " << strerror(errno);
            return Status::GENERIC_ERROR;
        }

        m_implData->videoBuffers[m_implData->nVideoBuffers].start =
            mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE,
                 MAP_SHARED, m_implData->fd, buf.m.planes[0].m.mem_offset);

        if (m_implData->videoBuffers[m_implData->nVideoBuffers].start ==
            MAP_FAILED) {
            LOG(WARNING) << "mmap error "
                         << "errno: " << errno << " error: " << strerror(errno);
            return Status::GENERIC_ERROR;
        }

        m_implData->videoBuffers[m_implData->nVideoBuffers].length =
            buf.m.planes[0].length;
    }

    m_implData->frameDetails = details;

    return status;
}

aditof::Status LocalDevice::program(const uint8_t *firmware, size_t size) {
    using namespace aditof;
    Status status = Status::OK;

    static struct v4l2_ext_control extCtrl;
    static struct v4l2_ext_controls extCtrls;
    static unsigned char buf[CTRL_PACKET_SIZE];
    size_t readBytes = 0;

    if (size <= CTRL_PACKET_SIZE) {
        extCtrl.size = 2048 * sizeof(unsigned short);
        extCtrl.p_u16 = (unsigned short *)firmware;
        extCtrl.id = V4L2_CID_AD_DEV_SET_CHIP_CONFIG;
        memset(&extCtrls, 0, sizeof(struct v4l2_ext_controls));
        extCtrls.controls = &extCtrl;
        extCtrls.count = 1;

        if (xioctl(m_implData->sfd, VIDIOC_S_EXT_CTRLS, &extCtrls) == -1) {
            LOG(WARNING) << "Programming AFE error "
                         << "errno: " << errno << " error: " << strerror(errno);
            return Status::GENERIC_ERROR;
        }
    } else {
        while (readBytes < size) {

            if ((size - readBytes) >= CTRL_PACKET_SIZE) {
                extCtrl.size = 2048 * sizeof(unsigned short);
                extCtrl.p_u16 =
                    (unsigned short *)((char *)firmware + readBytes);
                extCtrl.id = V4L2_CID_AD_DEV_SET_CHIP_CONFIG;
                memset(&extCtrls, 0, sizeof(struct v4l2_ext_controls));
                extCtrls.controls = &extCtrl;
                extCtrls.count = 1;

                if (xioctl(m_implData->sfd, VIDIOC_S_EXT_CTRLS, &extCtrls) ==
                    -1) {
                    LOG(WARNING)
                        << "Programming AFE error "
                        << "errno: " << errno << " error: " << strerror(errno);
                    return Status::GENERIC_ERROR;
                }
                readBytes += CTRL_PACKET_SIZE;
                usleep(100);
            } else {
                memset(buf, 0, CTRL_PACKET_SIZE);
                memcpy(buf, ((const char *)firmware + readBytes),
                       size - readBytes);
                extCtrl.size = 2048 * sizeof(unsigned short);
                extCtrl.p_u16 = (unsigned short *)buf;
                extCtrl.id = V4L2_CID_AD_DEV_SET_CHIP_CONFIG;
                memset(&extCtrls, 0, sizeof(struct v4l2_ext_controls));
                extCtrls.controls = &extCtrl;
                extCtrls.count = 1;

                if (xioctl(m_implData->sfd, VIDIOC_S_EXT_CTRLS, &extCtrls) ==
                    -1) {
                    LOG(WARNING)
                        << "Programming AFE error "
                        << "errno: " << errno << " error: " << strerror(errno);
                    return Status::GENERIC_ERROR;
                }
                readBytes += CTRL_PACKET_SIZE;
                usleep(100);
            }
        }
    }

    return status;
}

aditof::Status LocalDevice::getFrame(uint16_t *buffer) {
    using namespace aditof;
    Status status = Status::OK;

    fd_set fds;
    struct timeval tv;
    int r;
    int i, j;
    struct v4l2_buffer buf;
    unsigned char *pdata;

    unsigned int width;
    unsigned int height;
    unsigned int offset[2];
    unsigned int offset_idx;

    FD_ZERO(&fds);
    FD_SET(m_implData->fd, &fds);

    tv.tv_sec = 4;
    tv.tv_usec = 0;

    r = select(m_implData->fd + 1, &fds, NULL, NULL, &tv);

    if (r == -1) {
        LOG(WARNING) << "select error "
                     << "errno: " << errno << " error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    } else if (r == 0) {
        LOG(WARNING) << "select timeout";
        return Status::GENERIC_ERROR;
    }

    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = m_implData->planes;

    if (xioctl(m_implData->fd, VIDIOC_DQBUF, &buf) == -1) {
        LOG(WARNING) << "VIDIOC_DQBUF error "
                     << "errno: " << errno << " error: " << strerror(errno);
        switch (errno) {
        case EAGAIN:
        case EIO:
            break;
        default:
            return Status::GENERIC_ERROR;
        }
    }

    if (buf.index >= m_implData->nVideoBuffers) {
        LOG(WARNING) << "Not enough buffers avaialable";
        return Status::GENERIC_ERROR;
    }

    j = 0;

    width = m_implData->frameDetails.width;
    height = m_implData->frameDetails.height;
    pdata = (unsigned char *)m_implData->videoBuffers[buf.index].start;
    offset[0] = 0;
    offset[1] = height * width / 2;
    if ((width == 668)) {
        for (i = 0; i < (int)(height * width * 3 / 2); i += 3) {
            if ((i != 0) && (i % (336 * 3) == 0)) {
                j -= 4;
            }

            buffer[j] = (((unsigned short)*(pdata + i)) << 4) |
                        (((unsigned short)*(pdata + i + 2)) & 0x000F);
            j++;

            buffer[j] = (((unsigned short)*(pdata + i + 1)) << 4) |
                        ((((unsigned short)*(pdata + i + 2)) & 0x00F0) >> 4);
            j++;
        }
    } else {
        for (i = 0; i < (int)(height * width * 3 / 2); i += 3) {

            offset_idx = ((j / width) % 2);

            buffer[offset[offset_idx]] =
                (((unsigned short)*(pdata + i)) << 4) |
                (((unsigned short)*(pdata + i + 2)) & 0x000F);
            offset[offset_idx]++;

            buffer[offset[offset_idx]] =
                (((unsigned short)*(pdata + i + 1)) << 4) |
                ((((unsigned short)*(pdata + i + 2)) & 0x00F0) >> 4);
            offset[offset_idx]++;

            j += 2;
        }
    }

    if (xioctl(m_implData->fd, VIDIOC_QBUF, &buf) == -1) {
        LOG(WARNING) << "VIDIOC_QBUF error "
                     << "errno: " << errno << " error: " << strerror(errno);
        return Status::GENERIC_ERROR;
    }

    return status;
}

aditof::Status LocalDevice::readEeprom(uint32_t address, uint8_t *data,
                                       size_t length) {
    using namespace aditof;
    Status status = Status::OK;

    eeprom edev;

    if (eeprom_open(EEPROM_DEV_PATH, &edev) < 0) {
        LOG(WARNING) << "EEPROM open error";
        return Status::GENERIC_ERROR;
    }

    int ret = eeprom_read_buf(&edev, address, data, length);
    if (ret == -1) {
        LOG(WARNING) << "EEPROM read error";
        return Status::GENERIC_ERROR;
    }

    eeprom_close(&edev);

    return status;
}

aditof::Status LocalDevice::writeEeprom(uint32_t address, const uint8_t *data,
                                        size_t length) {
    using namespace aditof;
    Status status = Status::OK;

    eeprom edev;

    if (eeprom_open(EEPROM_DEV_PATH, &edev) < 0) {
        LOG(WARNING) << "EEPROM open error";
        return Status::GENERIC_ERROR;
    }

    int ret =
        eeprom_write_buf(&edev, address, const_cast<uint8_t *>(data), length);
    if (ret == -1) {
        LOG(WARNING) << "EEPROM write error";
        return Status::GENERIC_ERROR;
    }

    eeprom_close(&edev);

    return status;
}

aditof::Status LocalDevice::readAfeRegisters(const uint16_t *address,
                                             uint16_t *data, size_t length) {
    using namespace aditof;
    Status status = Status::OK;

    static struct v4l2_ext_control extCtrl;
    static struct v4l2_ext_controls extCtrls;

    extCtrl.size = 2048 * sizeof(unsigned short);

    for (size_t i = 0; i < length; i++) {
        extCtrl.p_u16 = const_cast<uint16_t *>(&address[i]);
        extCtrl.id = V4L2_CID_AD_DEV_READ_REG;
        memset(&extCtrls, 0, sizeof(struct v4l2_ext_controls));
        extCtrls.controls = &extCtrl;
        extCtrls.count = 1;

        if (xioctl(m_implData->sfd, VIDIOC_S_EXT_CTRLS, &extCtrls) == -1) {
            LOG(WARNING) << "Programming AFE error "
                         << "errno: " << errno << " error: " << strerror(errno);
            return Status::GENERIC_ERROR;
        }
        data[i] = *extCtrl.p_u16;
    }

    return status;
}

aditof::Status LocalDevice::writeAfeRegisters(const uint16_t *address,
                                              const uint16_t *data,
                                              size_t length) {
    using namespace aditof;
    Status status = Status::OK;

    static struct v4l2_ext_control extCtrl;
    static struct v4l2_ext_controls extCtrls;
    static unsigned char buf[CTRL_PACKET_SIZE];
    unsigned short sampleCnt = 0;

    length *= 2 * sizeof(unsigned short);
    while (length) {
        memset(buf, 0, CTRL_PACKET_SIZE);
        for (size_t i = 0;
             i < (length > CTRL_PACKET_SIZE ? CTRL_PACKET_SIZE : length);
             i += 4) {
            *(unsigned short *)(buf + i) = address[sampleCnt];
            *(unsigned short *)(buf + i + 2) = data[sampleCnt];
            sampleCnt++;
        }
        length -= CTRL_PACKET_SIZE;

        extCtrl.size = 2048 * sizeof(unsigned short);
        extCtrl.p_u16 = (unsigned short *)buf;
        extCtrl.id = V4L2_CID_AD_DEV_SET_CHIP_CONFIG;
        memset(&extCtrls, 0, sizeof(struct v4l2_ext_controls));
        extCtrls.controls = &extCtrl;
        extCtrls.count = 1;

        if (xioctl(m_implData->sfd, VIDIOC_S_EXT_CTRLS, &extCtrls) == -1) {
            LOG(WARNING) << "Programming AFE error "
                         << "errno: " << errno << " error: " << strerror(errno);
            return Status::GENERIC_ERROR;
        }
    }

    return status;
}

aditof::Status LocalDevice::readAfeTemp(float &temperature) {
    using namespace aditof;
    Status status = Status::OK;

    temp_sensor tdev;

    if (temp_sensor_open(TEMP_SENSOR_DEV_PATH, AFE_TEMP_SENSOR_I2C_ADDR,
                         &tdev) < 0) {
        LOG(WARNING) << "Temp sensor open error";
        return Status::GENERIC_ERROR;
    }

    if (temp_sensor_read(&tdev, &temperature) == -1) {
        LOG(WARNING) << "Error reading AFE_TEMP_SENSOR";
        return Status::GENERIC_ERROR;
    }

    temp_sensor_close(&tdev);

    return status;
}

aditof::Status LocalDevice::readLaserTemp(float &temperature) {
    using namespace aditof;
    Status status = Status::OK;

    temp_sensor tdev;

    if (temp_sensor_open(TEMP_SENSOR_DEV_PATH, LASER_TEMP_SENSOR_I2C_ADDR,
                         &tdev) < 0) {
        LOG(WARNING) << "Temp sensor open error";
        return Status::GENERIC_ERROR;
    }

    if (temp_sensor_read(&tdev, &temperature) == -1) {
        LOG(WARNING) << "Error reading LASER_TEMP_SENSOR";
        return Status::GENERIC_ERROR;
    }

    temp_sensor_close(&tdev);

    return status;
}