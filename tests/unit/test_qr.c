#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "../test_framework.h"

#include "../../src/util/buffer.h"
#include "../../src/util/arena.h"
#include "../../src/compress/deflate.h"
#include "../../src/image/png_decoder.h"
#include "../../src/image/jpeg_codec.h"
#include "../../src/image/ccitt_codec.h"
#include "../../src/pdf/pdf_base14.h"
#include "../../src/layout/layout.h"
#include "../../src/pdf/tspdf_writer.h"
#include "../../src/tspdf_error.h"
#include "../../src/qr/qr_encode.h"
#include "../../src/util/pdfdate.h"


// Byte-mode character capacity per level per ISO/IEC 18004 Table 7,
// cross-checked against segno at dev time (the smallest version segno
// picks for a payload of exactly this length is the same version).
static const int qr_expected_char_capacity[4][12] = {
    [QR_EC_L] = {0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271, 321},
    [QR_EC_M] = {0, 14, 26, 42, 62, 84, 106, 122, 152, 180, 213, 251},
    [QR_EC_Q] = {0, 11, 20, 32, 46, 60, 74, 86, 108, 130, 151, 177},
    [QR_EC_H] = {0, 7, 14, 24, 34, 44, 58, 64, 84, 98, 119, 137},
};

TEST(test_qr_ecc_table_consistent) {
    // Every row of every level's block table must satisfy
    // blocks x (data + ec) == total codewords for the version, and the
    // derived character capacity must match ISO 18004 Table 7.
    ASSERT(qr_max_version() >= 10);
    ASSERT(qr_max_version() + 1 <=
           (int)(sizeof(qr_expected_char_capacity[0]) / sizeof(int)));
    static const QrEcLevel levels[] = {QR_EC_L, QR_EC_M, QR_EC_Q, QR_EC_H};
    for (size_t li = 0; li < 4; li++) {
        QrEcLevel level = levels[li];
        for (int v = 1; v <= qr_max_version(); v++) {
            int total, ec, b1, d1, b2, d2;
            ASSERT_EQ_INT(qr_ecc_block_info(v, level, &total, &ec, &b1, &d1,
                                            &b2, &d2), 0);
            ASSERT(b1 >= 1);
            ASSERT_EQ_INT(b1 * (d1 + ec) + b2 * (d2 + ec), total);
            // Group 2 blocks, when present, carry exactly one more data
            // codeword.
            if (b2 > 0) ASSERT_EQ_INT(d2, d1 + 1);
            // Total codewords for a version is fixed by the module count:
            // it must match the ISO totals independent of the block split.
            static const int totals[] = {0, 26, 44, 70, 100, 134, 172,
                                         196, 242, 292, 346, 404};
            ASSERT_EQ_INT(total, totals[v]);
            int data_cw = b1 * d1 + b2 * d2;
            int cc_bits = (v <= 9) ? 8 : 16;
            int capacity = (data_cw * 8 - 4 - cc_bits) / 8;
            ASSERT_EQ_INT(capacity, qr_expected_char_capacity[level][v]);
        }
    }
    ASSERT_EQ_INT(qr_ecc_block_info(0, QR_EC_M, 0, 0, 0, 0, 0, 0), -1);
    ASSERT_EQ_INT(qr_ecc_block_info(qr_max_version() + 1, QR_EC_M,
                                    0, 0, 0, 0, 0, 0), -1);
    ASSERT_EQ_INT(qr_ecc_block_info(1, (QrEcLevel)4, 0, 0, 0, 0, 0, 0), -1);
}

TEST(test_qr_rs_known_vector) {
    // The classic v1-M "HELLO WORLD" example (alphanumeric mode): its data
    // codewords and 10 EC codewords are published in the ISO spec walkthroughs.
    static const uint8_t data[16] = {32, 91, 11, 120, 209, 114, 220, 77,
                                     67, 64, 236, 17, 236, 17, 236, 17};
    static const uint8_t expected[10] = {196, 35, 39, 119, 235,
                                         215, 231, 226, 93, 23};
    uint8_t ecc[10];
    qr_rs_ecc(data, 16, ecc, 10);
    ASSERT(memcmp(ecc, expected, sizeof(expected)) == 0);
}

/* RS reference: 26 EC codewords over the 44-byte block below (the v3-M
 * block shape), generated at dev time with an independent Python
 * GF(256)/0x11D Reed-Solomon implementation. */
static const char qr_rs_ref_data[45] = "tspdf reed-solomon reference vector, 44 byte";
static const uint8_t qr_rs_ref_ecc[26] = {
    7, 203, 226, 141, 139, 144, 176, 54, 193, 195, 192, 143, 247,
    134, 77, 167, 60, 75, 46, 30, 5, 203, 173, 213, 182, 230,
};

TEST(test_qr_rs_reference_vector) {
    uint8_t ecc[26];
    qr_rs_ecc((const uint8_t *)qr_rs_ref_data, 44, ecc, 26);
    ASSERT(memcmp(ecc, qr_rs_ref_ecc, sizeof(qr_rs_ref_ecc)) == 0);
}

TEST(test_qr_version_info_bits) {
    // BCH(18,6) values from ISO/IEC 18004:2015 Annex D, Table D.1.
    ASSERT(qr_version_info_bits(1) == 0);
    ASSERT(qr_version_info_bits(6) == 0);
    ASSERT(qr_version_info_bits(7) == 0x07C94);
    ASSERT(qr_version_info_bits(8) == 0x085BC);
    ASSERT(qr_version_info_bits(9) == 0x09A99);
    ASSERT(qr_version_info_bits(10) == 0x0A4D3);
    ASSERT(qr_version_info_bits(11) == 0x0BBF6);
}

TEST(test_qr_version_selection_boundaries) {
    // Smallest version is chosen by the byte-mode character capacity at
    // the requested level (default M).
    struct { int len; int size; QrEcLevel level; } cases[] = {
        {14, 21, QR_EC_M},   // v1 max
        {15, 25, QR_EC_M},   // spills to v2
        {42, 29, QR_EC_M},   // v3 max
        {43, 33, QR_EC_M},   // spills to v4
        {213, 57, QR_EC_M},  // v10 max
        {214, 61, QR_EC_M},  // spills to v11
        {251, 61, QR_EC_M},  // v11 max
        {17, 21, QR_EC_L},   // v1 max at L (more room than M)
        {18, 25, QR_EC_L},   // spills to v2
        {321, 61, QR_EC_L},  // v11 max at L
        {11, 21, QR_EC_Q},   // v1 max at Q
        {12, 25, QR_EC_Q},   // spills to v2
        {7, 21, QR_EC_H},    // v1 max at H (least room)
        {8, 25, QR_EC_H},    // spills to v2
        {137, 61, QR_EC_H},  // v11 max at H
    };
    char buf[400];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(buf, 'a', (size_t)cases[i].len);
        buf[cases[i].len] = '\0';
        QrCode *qr = qr_encode_level(buf, cases[i].level);
        ASSERT(qr != NULL);
        ASSERT_EQ_INT(qr->size, cases[i].size);
        qr_free(qr);
    }
    memset(buf, 'a', 252);
    buf[252] = '\0';
    ASSERT(qr_encode(buf) == NULL);   // beyond v11 capacity at M
    memset(buf, 'a', 138);
    buf[138] = '\0';
    ASSERT(qr_encode_level(buf, QR_EC_H) == NULL);  // beyond v11 at H
    memset(buf, 'a', 322);
    buf[322] = '\0';
    ASSERT(qr_encode_level(buf, QR_EC_L) == NULL);  // beyond v11 at L
    ASSERT(qr_encode_level("x", (QrEcLevel)4) == NULL);  // bad level
}

/*
 * Golden module grids. Each row is a bitmap with bit c = column c
 * (1 = dark). Generated from this encoder and decode-verified at dev time
 * (OpenCV cv2.QRCodeDetector for the level-M grids, zxing-cpp for the
 * L/H grids): every grid below decoded back to its exact payload. In
 * addition, full-capacity payloads at every version and level were
 * byte-identical to segno's canonical matrices. The grids pin the whole
 * pipeline — version selection, data encoding, RS interleaving, matrix
 * layout, format and version info, mask choice — so any behavior change
 * shows up here.
 */
typedef struct {
    const char *payload;
    QrEcLevel level;
    int size;
    const uint64_t rows[61];
} QrGolden;

static const QrGolden qr_goldens[] = {
    { /* len 10, version 1 */
      "https://ex",
      QR_EC_M, 21,
        { 0x1fc37f, 0x105141, 0x174a5d, 0x17555d, 0x17465d, 0x104641, 0x1fd57f, 0x500,
          0x1a44ed, 0x1ff792, 0x1a31cc, 0xa90b3, 0x134cc1, 0x16100, 0x1017f, 0x17b941,
          0x52e5d, 0x8e55d, 0x48f5d, 0x11d841, 0x73d7f } },
    { /* len 30, version 3 */
      "https://example.org/p/abcdefgh",
      QR_EC_M, 29,
        { 0x1fcd387f, 0x105bd141, 0x1749785d, 0x1744e65d, 0x175c555d, 0x10482241,
          0x1fd5557f, 0xb6600, 0x902d855, 0x1252c03b, 0x1dc62262, 0x9369914,
          0x1a7b0ed3, 0x1263a0b4, 0x1ba1de7d, 0xab37996, 0x1a72ced0, 0x1672cca2,
          0x19703fc9, 0xb868f3a, 0x1f31c4d, 0x1d13b700, 0x1b5bc87f, 0x1b137a41,
          0x9f3c95d, 0x532ce5d, 0x138b295d, 0x85ec041, 0x199bd17f } },
    { /* len 60, version 4 */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_M, 33,
        { 0x1fcad2e7f, 0x10505f841, 0x17561235d, 0x17542d75d, 0x1749ea55d, 0x105628341,
          0x1fd55557f, 0x1a88f00, 0x7c5a687d, 0x16cbd198c, 0xd2769ef4, 0xf63d5c1c,
          0x3bca8ef3, 0x1c4bd5d85, 0x9e75a0ef, 0x67610799, 0x11b0afdf6, 0x16cad730c,
          0xdc77eeec, 0x17979d216, 0x13b42bee0, 0x1449d45a7, 0xa346e47d, 0x63a55301,
          0x19fca8cc1, 0x1513d2300, 0xd5c0b67f, 0xf1adab41, 0x13f42ab5d, 0x1f31d515d,
          0x4e57b75d, 0x7479b041, 0x8bd2ef7f } },
    { /* len 116, version 7 */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcd",
      QR_EC_M, 45,
        { 0x1fd26229c47f, 0x10499dd8bc41, 0x174b5c0c555d, 0x1758a577d35d,
          0x175c6bf8cd5d, 0x10429d12ab41, 0x1fd55555557f, 0x17f107500, 0x7c0cffb907d,
          0x1919f30d1235, 0x8f4072f5df0, 0x7a77d02728c, 0x1002e3ee715d, 0x143962404280,
          0xe4e1df166ca, 0xfbf7ccc0431, 0x20e53ec2f0, 0x14b84a286524, 0x84e1dd12a57,
          0x17af780e5107, 0x11f6c3f7cff4, 0x1719f318651a, 0xd5e055c8559, 0xf1779161119,
          0x13f2e1f5a1fa, 0x1451797f830a, 0xf3e0c12bbed, 0x6cf7db20813, 0x94085811d54,
          0x1448eb72da15, 0x8961c0a2bc0, 0x16cfb99dd6be, 0x11b68288b877,
          0x1428f37e270d, 0xab78427f550, 0xe5b79984f1e, 0x11f0e5f1b659, 0x17197b161f00,
          0xd5e05584e7f, 0x71b771f2341, 0x13f887f6695d, 0x1da0f22a095d, 0x85e150c2d5d,
          0x72f78125c41, 0x8dea3ae257f } },
    { /* len 150, version 8 */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_M, 49,
        { 0x1fd107a0e4c7f, 0x105c69c6bd041, 0x175853a71d35d, 0x174bea5a8535d,
          0x17438e7e25d5d, 0x104468c481141, 0x1fd555555557f, 0x1a31c40b900,
          0x7ddec7f29a7d, 0x639f30882b4, 0x191d68dec00c4, 0x90a77a01288f,
          0x1e99601e3876e, 0x8c38639b8886, 0x1b15e0597cf40, 0x11487781c0708,
          0x69f8e5f75bd8, 0xe29e2adaf28, 0x1a05e1d9169fb, 0x980558111db3,
          0x1413ce3f52b46, 0x7e307be9e012, 0x1bf449ff3f9f6, 0x111a31c44951e,
          0x1f57ce5753152, 0x11a17c4b0118, 0x1ffc68fc6f1f2, 0x123fc292b80,
          0x1f1b8a246725b, 0xeb1efe72c05, 0x19ecf82c61d74, 0x9277d35082c,
          0x1f29e825290c1, 0xa216bafa899, 0x1a4de1b4fd5d2, 0x11635db33a06,
          0x154be801605ef, 0x6ba9e7af428e, 0x196fe1a4b8ce2, 0x91411d820c0e,
          0x17f9a87f102c7, 0x1138e44fc100, 0x1d5468d7e1c7f, 0x191231c60c341,
          0x1ff5e47d0435d, 0x19db1f06cd55d, 0x6e568dfe675d, 0x107a37b2c9441,
          0x1e1b8a411937f } },
    { /* len 200, version 10 */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_M, 57,
        { 0x1fcee38fbebf07f, 0x104a9c7050cfe41, 0x174f1e2ddda115d, 0x1748c7e02112d5d,
          0x17487a0fdff3d5d, 0x104787e47abe741, 0x1fd55555555557f, 0x1581471db100,
          0x7c0e38fc3dc27d, 0x1639f29f1858093, 0xcdc0d68d467c7c, 0xfbf7a16268663a,
          0x6c1aed45c34a, 0x16b97b066be9411, 0xbc71de8c6e7e4e, 0x7937a11ed85f1d,
          0x1064efcc0179052, 0x1019d38e3e83296, 0x87e8560867b1cf, 0xf217c73ba32528,
          0x104685e84105b7a, 0x10b86b02bd1d8aa, 0xcce9c75ec97561, 0x17cf1821b04ab29,
          0xa20e1d8410b1dd, 0x153861965d12405, 0x9fe05ffe3c4bf4, 0x1f1f581443f411f,
          0x15a85ed470f55c, 0x17117a147c1d715, 0xff60d6fee41ff7, 0x16cb7a1018a8e2b,
          0x1190e38dbc02fc1, 0x40e29a50c4e39, 0x1db71d67fe0ea51, 0x7c93c709200bb5,
          0x1a281ae80d8563, 0x14317a115cdc715, 0xf368c779acecc6, 0x64978315928f89,
          0x11d4a389c1d0b78, 0x1651f29e5eebbbd, 0xc9e05efe6ee0fb, 0xe5f9e01b703700,
          0xb2687a5814f3cc, 0x1468eb061b99814, 0xabf85f57dc8065, 0x164b5816016d21f,
          0x1f0a7cfc0a3f40, 0x1710e38c5b35100, 0x95e0d6d642b07f, 0xf1b7a145989f41,
          0x11f685eff4ad15d, 0x1c06a15c98815d, 0x65f1d6627e695d, 0x7a934768c70441,
          0x8d0a388229a57f } },
    { /* len 250, version 11 — the only version where group 1 has FEWER
       * blocks than group 2 (1x50 + 4x51), so this pins that interleave
       * shape end to end. */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefgh",
      QR_EC_M, 61,
        { 0x1fdb8f2965949e7f, 0x1059e056a8fe1a41, 0x175c15c0e599d55d,
          0x1757e81b1be8515d, 0x174f17a1f55f3f5d, 0x1045f05718d34141,
          0x1fd555555555557f, 0x1c53a315431b00, 0x7d9ea5ff279da7d, 0x7e28e396c362480,
          0x1e057056b8c8cb42, 0x19fc11a2672a2132, 0x659205f1ac2d7e7,
          0x3e386390c06c20d, 0x180de056da8c235a, 0x117c15a3e11d9799,
          0x6fb8e7c82e45d40, 0x6e38f290c61dd97, 0x1a95e056fb9a91cb,
          0x16415c16217ca2b, 0x495e81a831695df, 0x66317a10d0f881a,
          0x1885f0569815046d, 0x10015c166f02e9c, 0x69bca3893058d51,
          0x7c28e390cd77aa3, 0x1e057056d817b950, 0x197855a266c16715,
          0x7fbce7dfa1cd1f3, 0x31206a11598b916, 0x195550475b776d51,
          0x111c1da31839e716, 0x7fb8e7dfa6265f8, 0x2cb1ea115fecba7,
          0x1f64f8471a8a824e, 0x81415c0cba6a415, 0x1ffde81bea344c69,
          0x8f317a09595012b, 0xa6df0570a4a0bd5, 0x101015c0e5b5c513,
          0x156bca39636184c3, 0x6928e389575a23c, 0x1e6d70570b086f51,
          0x189077c080294eb7, 0x77dac5ee27b9ff6, 0x2b206a095bce803,
          0x19e570474aa807ca, 0x101635c0e1b60903, 0x72be81aea66246f,
          0x2ab9f38959e2cbb, 0x1f4768c71a7b717c, 0x81415c0ebc31e97,
          0x1ffde01bfa0c9ccf, 0x3131f391db02d00, 0x1d5568c753ed687f,
          0x11015c115977f41, 0x1ffbca39fa5d5f5d, 0x117a8e39fdd1ad5d,
          0x12e570569bfd0f5d, 0x10f877c11f842641, 0x1c05ac5ee25b9d7f } },
    { /* len 230, level L, version 9 (2x116 data codewords per block — the
       * largest blocks in the supported range; they would have overflowed
       * the level-M-era 64-byte block buffers). */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_L, 53,
        { 0x1fc6a97be8507f, 0x104e5f06b74b41, 0x1749c363f68c5d, 0x17565c8c17bd5d,
          0x174620ffe8485d, 0x1045d691652741, 0x1fd5555555557f, 0xda751ae0e00,
          0xab03aff17b5df, 0x150f20f9684a93, 0xe8df042fc57e, 0xa61817bd0bd33,
          0xdb43aa417becd, 0x188eb87de842b2, 0x7f1d6807dba7e, 0x100da75b8026bc,
          0x792589c17a84d, 0x152fb878685022, 0x378df06065dc2, 0xa798103b2cc05,
          0x59658c597b75f, 0x1c0f21fde84d0c, 0x5e84602743067, 0x18cfa743e2552d,
          0x5f47ebf97a3ff, 0x151e31f1685b12, 0x550de95199d54, 0xb1181118db31e,
          0x5f07eff17bdf0, 0x1fe7b97ce85d92, 0x780461b2ea7ca, 0xffc360046621,
          0x46618cb17b556, 0x11f7a964e84f1a, 0x391d69d1005e8, 0xbfda1056fc3b3,
          0x1c561c8b97bac1, 0x1fde30fce85fa0, 0x788461a37436d, 0xb5fc363f6968c,
          0x4303ef3178ce6, 0x198e20ece801b4, 0x501d69808b07b, 0x1beda742c9e886,
          0x5f03aff1784c8, 0x1b1ea971684900, 0x3585f15afa77f, 0x131fc371d0e241,
          0xff25c9f17f95d, 0x1836b87268455d, 0x17e9d68565535d, 0xb15a75a2e1341,
          0x462588597dd7f } },
    { /* len 60, level H, version 7 (4x13 + 1x14: a single group-2 block,
       * plus level-H format info bits and v7 version info blocks). */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_H, 45,
        { 0x1fd0ec3de97f, 0x104a14cca341, 0x174ac221875d, 0x17580d36265d,
          0x175cf5f7565d, 0x10421f12d141, 0x1fd55555557f, 0x1d6d11db00,
          0x1ce4e3fb535c, 0x113be613929e, 0x8ce853bf751, 0x1f950c4d33a2,
          0x804df2134f0, 0x11b8736247ae, 0xde48ec921f0, 0x7e32da6ffa2, 0xa609e63c2c8,
          0x1330a47b9cbe, 0xc6e431f9a4b, 0x7d954823b07, 0x1f2f5f9a7fd,
          0x17184b1de717, 0xf577d5cd95c, 0x7106b121d18, 0x19f5c5f4d3f9,
          0x101929155927, 0xdbfb29e627e, 0xf4d5d633e1a, 0x1104ff705d55,
          0x142139c12826, 0xeb6e5eee74a, 0x164b738fd185, 0x9b289dd6973,
          0x161bc282ac8d, 0xdb62222b350, 0x1ecb19489d1e, 0x3f6e7f21e59,
          0x1f1991119700, 0xf57df5ae27f, 0xd11eb188641, 0x19f8c7fca95d,
          0x1f80eb1cd35d, 0x7edbe8c55d, 0x6062b655e41, 0x9f2737f3c7f } },
};

TEST(test_qr_golden_grids) {
    for (size_t g = 0; g < sizeof(qr_goldens) / sizeof(qr_goldens[0]); g++) {
        const QrGolden *gd = &qr_goldens[g];
        QrCode *qr = qr_encode_level(gd->payload, gd->level);
        ASSERT(qr != NULL);
        ASSERT_EQ_INT(qr->size, gd->size);
        for (int r = 0; r < qr->size; r++) {
            uint64_t bits = 0;
            for (int c = 0; c < qr->size; c++) {
                if (qr->modules[r * qr->size + c])
                    bits |= (uint64_t)1 << c;
            }
            if (bits != gd->rows[r]) {
                qr_free(qr);
                printf("FAIL\n    golden %zu (\"%.20s...\") row %d: "
                       "got 0x%llx want 0x%llx\n",
                       g, gd->payload, r,
                       (unsigned long long)bits,
                       (unsigned long long)gd->rows[r]);
                tests_failed++;
                _test_failed = true;
                return;
            }
        }
        qr_free(qr);
    }
}

void run_qr_tests(void) {
    printf("\n  QR encoder:\n");
    RUN(test_qr_ecc_table_consistent);
    RUN(test_qr_rs_known_vector);
    RUN(test_qr_rs_reference_vector);
    RUN(test_qr_version_info_bits);
    RUN(test_qr_version_selection_boundaries);
    RUN(test_qr_golden_grids);
}
