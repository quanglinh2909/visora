// The persistence adapters' own logic — the parts that are NOT a database call.
//
// The `ai_jobs.stages` column is the whole of it today, and it earns a test
// because its shape is a shared contract: the predecessor writes and reads the
// same column, so a deployment can be rolled back to it. Getting the encoding
// subtly wrong would not fail here; it would fail after someone stopped Visora
// at three in the morning.

#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "store/AiJobJson.hpp"

using namespace visora;

VS_TEST(class_filters_are_stored_the_way_the_predecessor_stores_them) {
    // Comma-separated strings, not JSON arrays. Visora's API speaks arrays,
    // which is nicer, but the column is shared with a system that may be
    // started again after a rollback.
    VS_CHECK(store::formatClassList({}) == "");
    VS_CHECK(store::formatClassList({2}) == "2");
    VS_CHECK(store::formatClassList({2, 3, 5, 7}) == "2,3,5,7");

    // "all" and "" both mean no filter, which is what an operator who has not
    // thought about classes yet has.
    VS_CHECK(store::parseClassList("").empty());
    VS_CHECK(store::parseClassList("all").empty());

    const auto parsed = store::parseClassList("2,3,5,7");
    VS_CHECK_EQ(parsed.size(), std::size_t{4});
    if (parsed.size() == 4) VS_CHECK_EQ(parsed[3], 7);

    // One unreadable token must not make the whole field unreadable.
    const auto messy = store::parseClassList("2,,oops,5");
    VS_CHECK_EQ(messy.size(), std::size_t{2});
    if (messy.size() == 2) {
        VS_CHECK_EQ(messy[0], 2);
        VS_CHECK_EQ(messy[1], 5);
    }
}

VS_TEST(a_stage_tree_survives_a_round_trip_through_the_column) {
    std::vector<vision::AiStage> stages;
    vision::AiStage detect;
    detect.modelType = "yolov8_detect";
    detect.modelPath = "/models/yolov8n.rknn";
    detect.parent = -1;
    detect.classFilter = {2, 3, 5, 7};
    detect.confidence = 0.4f;
    stages.push_back(detect);

    vision::AiStage read;
    read.modelType = "paddle_ocr_rec";
    read.modelPath = "/models/plate_rec.rknn";
    read.parent = 0;
    read.inputClasses = {7};
    read.transform = "crop";
    read.confidence = 0.3f;
    stages.push_back(read);

    const std::string json = store::stagesToJson(stages);
    const auto back = store::stagesFromJson(json);

    VS_CHECK_EQ(back.size(), std::size_t{2});
    if (back.size() != 2) return;
    VS_CHECK(back[0].modelType == "yolov8_detect");
    VS_CHECK_EQ(back[0].parent, -1);
    VS_CHECK_EQ(back[0].classFilter.size(), std::size_t{4});
    VS_CHECK(back[1].modelType == "paddle_ocr_rec");
    VS_CHECK_EQ(back[1].parent, 0);
    VS_CHECK_EQ(back[1].inputClasses.size(), std::size_t{1});
    VS_CHECK(back[1].transform == "crop");
    VS_CHECK(back[1].confidence > 0.29f && back[1].confidence < 0.31f);
}

VS_TEST(a_row_written_by_the_predecessor_reads_correctly) {
    // Byte for byte what gstreamer_c stores, from the example in its own
    // sql/init.sql. If this stops passing, an existing deployment's jobs are
    // gone after the cutover.
    const std::string stored =
        R"([{"modelPath":"weights/yolov8.rknn","modelType":"yolov8_detect",)"
        R"("classFilter":"2,3,5,7","conf":0.25},)"
        R"({"parent":0,"modelPath":"weights/plate_det.rknn",)"
        R"("modelType":"paddle_ocr_det","inputClasses":"7","conf":0.3},)"
        R"({"parent":1,"modelPath":"weights/plate_rec.rknn",)"
        R"("modelType":"paddle_ocr_rec","conf":0.3}])";

    const auto stages = store::stagesFromJson(stored);
    VS_CHECK_EQ(stages.size(), std::size_t{3});
    if (stages.size() != 3) return;

    // No "parent" on the first stage means the whole frame.
    VS_CHECK_EQ(stages[0].parent, -1);
    VS_CHECK(stages[0].modelPath == "weights/yolov8.rknn");
    VS_CHECK_EQ(stages[0].classFilter.size(), std::size_t{4});
    VS_CHECK_EQ(stages[1].parent, 0);
    VS_CHECK_EQ(stages[1].inputClasses.size(), std::size_t{1});
    VS_CHECK_EQ(stages[2].parent, 1);
    // Absent transform is the plain crop, which resolves by name later.
    VS_CHECK(stages[2].transform.empty());
}

VS_TEST(a_stage_with_no_parent_field_chains_onto_the_one_before_it) {
    // What the predecessor did, and rows it wrote rely on it: a stage list with
    // no parents at all is a straight pipeline, not three roots.
    const std::string stored =
        R"([{"modelType":"yolov8_detect","modelPath":"a.rknn"},)"
        R"({"modelType":"paddle_ocr_rec","modelPath":"b.rknn"}])";
    const auto stages = store::stagesFromJson(stored);
    VS_CHECK_EQ(stages.size(), std::size_t{2});
    if (stages.size() != 2) return;
    VS_CHECK_EQ(stages[0].parent, -1);
    VS_CHECK_EQ(stages[1].parent, 0);
}

VS_TEST(an_unreadable_stage_tree_yields_no_stages_rather_than_an_error) {
    // A row whose JSON cannot be read is ONE job that will not start. Failing
    // the listing would hide every other job because of it.
    VS_CHECK(store::stagesFromJson("not json at all").empty());
    VS_CHECK(store::stagesFromJson("").empty());
    VS_CHECK(store::stagesFromJson("[]").empty());
}

VS_TEST(an_empty_tree_is_an_empty_array_which_is_what_the_column_defaults_to) {
    VS_CHECK(store::stagesToJson({}) == "[]");
}

VS_MAIN()
