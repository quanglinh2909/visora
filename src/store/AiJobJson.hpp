#pragma once

// The `ai_jobs.stages` column, both directions.
//
// A stage tree is stored as ONE jsonb value rather than as a table of stages,
// and that is deliberate: `parent` is an index into the array, so it only means
// anything in the context of the whole array. A row is a tree, read and written
// whole.
//
// THE SHAPE IS THE PREDECESSOR'S, exactly — `inputClasses` and `classFilter`
// are comma-separated STRINGS ("2,3,5,7", or "all" for everything), not JSON
// arrays. Visora's own API speaks arrays, which is nicer, but the column is
// shared with a system that may be started again after a rollback. Writing
// arrays into it would leave rows the predecessor cannot read, which turns "stop
// Visora, start gstreamer_c" from a rollback into an outage.
//
// This lives in store/ rather than in vision/ because it is a fact about the
// database, not about the domain.

#include <string>
#include <vector>

#include "core/Result.hpp"
#include "vision/AiJob.hpp"

namespace visora::store {

// Never fails: a job with no stages is an empty array, which is what the column
// defaults to.
std::string stagesToJson(const std::vector<vision::AiStage>& stages);

// Returns an empty list for an empty or unreadable value rather than an error.
//
// A row whose JSON cannot be read is a job that will not start, and the useful
// behaviour is for the OTHER jobs to start anyway and for this one to be
// visible and editable in the UI. Failing the whole listing would hide every
// job because of one bad row.
std::vector<vision::AiStage> stagesFromJson(const std::string& json);

// The CSV class filters, exposed because they are the part with a rule in them:
// "all" and "" both mean no filter, and a token that is not a number is
// skipped rather than making the whole field unreadable.
std::vector<int> parseClassList(const std::string& value);
std::string formatClassList(const std::vector<int>& classes);

}  // namespace visora::store
