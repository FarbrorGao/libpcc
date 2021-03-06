#ifndef LIBPCC_POINT_CLOUD_GRID_ENCODER_HPP
#define LIBPCC_POINT_CLOUD_GRID_ENCODER_HPP

#include "Encoder.hpp"
#include "PointCloudGrid.hpp"

#include <zmq.hpp>

#include <vector>
#include <string>
#include <sstream>
#include <iostream>
#include <map>

/**
 * Provides interface to point cloud compression
 * based on grid segmentation and adaptive quantization
 * of grid cells.
*/
class PointCloudGridEncoder : public Encoder {
public:
    /**
     * Data transfer object used for exposing
     * configurable settings for encoding process.
     * The value will not be changed from within this class,
     * thus it will only ever be changed by user.
    */
    struct EncodingSettings {
        EncodingSettings()
            : grid_precision()
            , verbose(false)
            , num_threads(24)
            , irrelevance_coding(true)
            , entropy_coding(true)
            , appendix_size(0)
        {}

        EncodingSettings(const EncodingSettings&) = default;

        Vec<float> const getQuantizationStepSize(size_t cell_idx) const
        {
            if(cell_idx >= grid_precision.point_precision.size() || cell_idx < 0) {
                std::cout << "NOTIFICATION: invalid cell_idx for call to getQuantizationStepSize" << std::endl;
                std::cout << "  > got:" << cell_idx;
                std::cout << "  > valid range: [0," << grid_precision.point_precision.size()-1 << "]." << std::endl;
            }

            BoundingBox bb(grid_precision.bounding_box);
            Vec8 dimensions(grid_precision.dimensions);
            Vec<int> num_quant_values(
                pow(2, static_cast<int>(grid_precision.point_precision[cell_idx].x)),
                pow(2, static_cast<int>(grid_precision.point_precision[cell_idx].y)),
                pow(2, static_cast<int>(grid_precision.point_precision[cell_idx].z))
            );

            Vec<float> quant_step;
            quant_step.x = ((bb.max.x-bb.min.x) / dimensions.x) / num_quant_values.x;
            quant_step.y = ((bb.max.y-bb.min.y) / dimensions.y) / num_quant_values.y;
            quant_step.z = ((bb.max.z-bb.min.z) / dimensions.z) / num_quant_values.z;
            return quant_step;
        }

        GridPrecisionDescriptor grid_precision;
        bool verbose;
        int num_threads;
        bool irrelevance_coding;
        bool entropy_coding;
        unsigned long appendix_size;
    };

    /**
     * Data transfer object used for storing encoding stats.
     */
    struct EncodeLog {
        time_t comp_time;
        time_t encode_time;
        time_t entropy_compress_time;
        size_t raw_byte_size;
        size_t comp_byte_size;
    };

    /**
     * Data transfer object used for storing decoding stats.
     */
    struct DecodeLog {
        time_t decomp_time;
        time_t decode_time;
        time_t entropy_decompress_time;
        size_t total_cell_header_size;
        size_t global_header_size;
        size_t black_list_size;
    };

    EncodingSettings settings;
    EncodeLog encode_log;
    DecodeLog decode_log;

private:
    template<typename C>
    using GridVec = std::vector<std::vector<Vec<C>>>;

    typedef std::map<Vec<uint64_t>, std::pair<Vec<uint64_t>, int>> PropertyMap;
    typedef std::pair<Vec<uint64_t>, std::pair<Vec<uint64_t>, int>> PropertyPair;

    /**
     * Data transfer object for encoding first chunk in a message.
     * Holds general info about encoded data,
     * such as whether or not entropy encoding has been performed
     * and how large the message appendix is.
    */
    struct GlobalHeader {
        GlobalHeader()
            : entropy_coding(false)
            , uncompressed_size(0)
            , appendix_size(0)
        {}

        bool entropy_coding;
        unsigned long uncompressed_size;
        unsigned long appendix_size;

        static size_t getByteSize()
        {
            return sizeof(bool) + 2*sizeof(unsigned long);
        }

        const std::string toString()
        {
            std::stringstream ss;
            ss << "GlobalHeader(entropy_coding = " << entropy_coding << ", ";
            ss << "uncompressed_size = " << uncompressed_size << ", ";
            ss << "appendix_size = " << appendix_size << ")";
            return ss.str();
        }
    };

    /**
     * Data transfer object for encoding general meta info about a PointCloudGrid.
     * Appears right after GlobalHeader, but might be entropy encoded.
    */
    struct GridHeader {
        Vec8 dimensions;
        BoundingBox bounding_box;
        unsigned num_blacklist;

        static size_t getByteSize()
        {
            return 3*sizeof(uint8_t) + 6*sizeof(float) + sizeof(unsigned);
        }

        const std::string toString() const
        {
            std::stringstream ss;
            ss << "GridHeader(dim=[" << (int) dimensions.x << "," << (int) dimensions.y << "," << (int) dimensions.z << "], ";
            ss << "bb={[" << bounding_box.min.x << "," << bounding_box.min.y << "," << bounding_box.min.z << "];";
            ss << "[" << bounding_box.max.x << "," << bounding_box.max.y << "," << bounding_box.max.z << "]}, ";
            ss << "num_bl=" << num_blacklist << ")";
            return ss.str();
        }
    };

    /**
     * Data transfer object for encoding meta info about a GridCell in a PointCloudGrid.
    */
    struct CellHeader {
        unsigned cell_idx; // not added to message (debug use)
        // TODO: pack
        BitCount point_encoding_x;
        BitCount point_encoding_y;
        BitCount point_encoding_z;
        BitCount color_encoding_x;
        BitCount color_encoding_y;
        BitCount color_encoding_z;
        unsigned num_elements;

        static size_t getByteSize()
        {
            return 1*sizeof(unsigned)+6*sizeof(BitCount);
        }

        const std::string toString() const
        {
            std::stringstream ss;
            ss << "CellHeader(c_idx=" << cell_idx << ", ";
            ss << "p_enc_x=" << point_encoding_x << ", ";
            ss << "p_enc_y=" << point_encoding_y << ", ";
            ss << "p_enc_z=" << point_encoding_z << ", ";
            ss << "c_enc_x=" << color_encoding_x << ", ";
            ss << "c_enc_y=" << color_encoding_y << ", ";
            ss << "c_enc_z=" << color_encoding_z << ", ";
            ss << "num_elmts=" << num_elements << ")";
            return ss.str();
        }
    };

public:
    explicit PointCloudGridEncoder(const EncodingSettings& s = EncodingSettings());
    ~PointCloudGridEncoder();

    /**
     * Compresses given UncompressedPointCloud and creates message from it.
     * Optionally num_points specifies how many UncompressedVoxels in point_cloud
     * will be encoded range [0,num_points-1]. If num_points < 0 all points will be
     * considered.
    */
    zmq::message_t encode(const std::vector<UncompressedVoxel>& point_cloud, int num_points=-1);

    /**
     * Decodes given message into point_cloud. Returns success.
    */
    bool decode(zmq::message_t& msg, std::vector<UncompressedVoxel>* point_cloud);

    /**
     * Returns a reference to the PointCloudGrid maintained by this instance.
     * After encode, this will contain the respective grid
     * setup from given std::vector<UncompressedVoxel>
     * PointCloudGridEncoder::settings. Value returned is read only.
     * Value will be 0 if no encode/decode performed yet.
    */
    const PointCloudGrid* getPointCloudGrid() const;

    /**
     * Inserts optional contents into given zmq::message_t.
     * Can be used to transmit arbitrary contents along with message.
     * msg has to be of format produced by encoder.
     * size can have maximum value as determined by
     * appendix_size in PointCloudGridEncoder::settings.appendix_size.
     * data should contain content to copy into appendix,
     * with size being the amount of bytes copied from data.
     * Returns success of operation.
    */
    bool writeToAppendix(zmq::message_t& msg, unsigned char* data, unsigned long size);

/**
     * Inserts optional contents into given zmq::message_t.
     * Can be used to transmit arbitrary contents along with message.
     * msg has to be of format produced by encoder.
     * size can have maximum value as determined by
     * appendix_size in PointCloudGridEncoder::settings.appendix_size.
     * Returns success of operation.
    */
    bool writeToAppendix(zmq::message_t& msg, const std::string& text);

    /**
     * Retrieves appendix contents from given zmq:message_t into data.
     * msg has to be of format produced by encoder.
     * Returns size of appendix.
    */
    unsigned long readFromAppendix(zmq::message_t& msg, unsigned char*& data);

    /**
     * Retrieves appendix contents from given zmq:message_t into text.
     * msg has to be of format produced by encoder.
    */
    void readFromAppendix(zmq::message_t& msg, std::string& text);

private:
    /**
     * Prepends GlobalHeader and adds space for appendix.
    */
    zmq::message_t finalizeMessage(zmq::message_t msg);

    /**
     * Compresses and returns given msg using zlib deflate.
    */
    zmq::message_t entropyCompression(zmq::message_t msg);

    /**
     * Decompressed and returns given msg using zlib inflate.
    */
    zmq::message_t entropyDecompression(zmq::message_t& msg, size_t offset);

    /**
     * Fills PointCloudGridEncoder::pc_grid_ from given point_cloud
     * and PointCloudGridEncoder::settings.
     * num_points specifies the number of points used in compression
     * from point_cloud.first() to point_cloud.first() + num_points.
    */
    void buildPointCloudGrid(const std::vector<UncompressedVoxel>& point_cloud, int num_points);

    /**
     * Extracts a uncompressed point cloud from PointCloudGridEncoder::pc_grid_.
     * Results are stored in given point_cloud.
     * Returns success of operation.
    */
    bool extractPointCloudFromGrid(std::vector<UncompressedVoxel>* point_cloud);

    /**
     * Creates a zmq message from current PointCloudGridEncoder::pc_grid_.
    */
    zmq::message_t encodePointCloudGrid();

    /**
     * Helper function for PointCloudGridEncoder::decode,
     * to extract a point cloud grid from given zmq message
     * into PointCloudGridEncoder::pc_grid_.
     * Returns success of operation.
    */
    bool decodePointCloudGrid(zmq::message_t& msg);

    /**
     * Helper function for PointCloudGridEncoder::encodePointCloudGrid.
     * Encodes the PointCloudGridEncoder::GlobalHeader for given msg
     * at msg.data() + offset.
     * Returns offset after extending msg.
    */
    size_t encodeGlobalHeader(zmq::message_t& msg, size_t offset = 0);

    /**
     * Helper function for PointCloudGridEncoder::decodePointCloudGrid.
     * Decodes the PointCloudGridEncoder::GlobalHeader for given msg
     * at msg.data() + offset.
     * Returns offset after decoding part of msg.
    */
    size_t decodeGlobalHeader(zmq::message_t& msg, size_t offset = 0);

    /**
     * Helper function for PointCloudGridEncoder::encodePointCloudGrid.
     * Encodes the PointCloudGridEncoder::GridHeader for given msg
     * at msg.data() + offset.
     * Returns offset after extending msg.
    */
    size_t encodeGridHeader(zmq::message_t& msg, size_t offset = 0);

    /**
     * Helper function for PointCloudGridEncoder::decodePointCloudGrid.
     * Decodes the PointCloudGridEncoder::GridHeader for given msg
     * at msg.data() + offset.
     * Returns offset after decoding part of msg.
    */
    size_t decodeGridHeader(zmq::message_t& msg, size_t offset = 0);

    /**
     * Helper function for PointCloudGridEncoder::encodePointCloudGrid.
     * Encodes the point cloud grid blacklist for given msg,
     * which contains the index of all cells not conatining any data.
     * Encoding is started at msg.data() + offset.
     * Returns offset after extending msg.
    */
    size_t encodeBlackList(zmq::message_t& msg, std::vector<unsigned> bl,
                           size_t offset);

    /**
     * Helper function for PointCloudGridEncoder::decodePointCloudGrid.
     * Decodes the point cloud grid blacklist for given msg,
     * which contains the index of all cells not containing any data.
     * Decoding is started at msg.data() + offset.
     * Returns offset after extracting from msg.
    */
    size_t decodeBlackList(zmq::message_t& msg, std::vector<unsigned>& bl,
                           size_t offset);

    /**
     * Helper function for PointCloudGridEncoder::encodePointCloudGrid.
     * Encodes given PointCloudGridEncoder::CellHeader into given msg
     * at msg.data() + offset.
     * Returns offset after extending msg.
    */
    size_t encodeCellHeader(zmq::message_t& msg, CellHeader* c_header, size_t offset);

    /**
     * Helper function for PointCloudGridEncoder::decodePointCloudGrid.
     * Decodes a PointCloudGridEncoder::CellHeader into given c_header
     * at msg.data() + offset.
     * Returns offset after extracting from msg.
    */
    size_t decodeCellHeader(zmq::message_t& msg, CellHeader* c_header, size_t offset);

    /**
     * Helper function for PointCloudGridEncoder::encodePointCloudGrid.
     * Encodes given PointCloudGrid::GridCell into given msg
     * at msg.data() + offset.
     * Returns offset after extending msg.
    */
    size_t encodeCell(zmq::message_t &msg, GridCell* cell, size_t offset);

    /**
     * Helper function for PointCloudGridEncoder::decodePointCloudGrid.
     * Decodes a PointCloudGrid::GridCell into PointCloudGridEncoder::pc_grid_
     * given meta data provided by CellHeader from given msg at msg.data() + offset.
     * Returns offset after extracting from msg.
    */
    size_t decodeCell(zmq::message_t &msg, CellHeader *c_header, size_t offset);

    /**
     * Calculates the index of the cell given point belongs to.
     * cell_range describes the x/y/z-sizes of a GridCell.
    */
    unsigned calcGridCellIndex(const float pos[3], const Vec<float>& cell_range) const;

    /**
     * Map given point into the local coordinate system of a cell.
    */
    const Vec<float> mapToCell(const float pos[3], const Vec<float>& cell_range);

    /**
     * Calculates the overall size of a poiunt cloud grid message in Bytes.
    */
    size_t calcMessageSize(const std::vector<CellHeader*>&) const;

    PointCloudGrid* pc_grid_;
    GridHeader* header_;
    GlobalHeader* global_header_;
};


#endif //LIBPCC_POINT_CLOUD_GRID_ENCODER_HPP
