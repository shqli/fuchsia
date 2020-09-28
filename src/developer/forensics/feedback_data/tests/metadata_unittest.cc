// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/metadata.h"

#include <lib/fit/result.h>
#include <lib/syslog/cpp/macros.h>

#include <cstring>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/developer/forensics/feedback_data/annotations/types.h"
#include "src/developer/forensics/feedback_data/attachments/types.h"
#include "src/developer/forensics/feedback_data/constants.h"
#include "src/developer/forensics/feedback_data/metadata_schema.h"
#include "src/developer/forensics/utils/errors.h"
// TODO(fxbug.dev/57392): Move it back to //third_party once unification completes.
#include "zircon/third_party/rapidjson/include/rapidjson/document.h"
#include "zircon/third_party/rapidjson/include/rapidjson/schema.h"

#define ANNOTATIONS_JSON_STATE_IS(json, state)                           \
  {                                                                      \
    ASSERT_TRUE(json.HasMember("files"));                                \
    auto& files = json["files"];                                         \
    ASSERT_TRUE(files.HasMember("annotations.json"));                    \
    ASSERT_TRUE(files["annotations.json"].HasMember("state"));           \
    EXPECT_STREQ(files["annotations.json"]["state"].GetString(), state); \
  }

#define HAS_PRESENT_ANNOTATION(json, name)                                                         \
  {                                                                                                \
    ASSERT_TRUE(json.HasMember("files"));                                                          \
    auto& files = json["files"];                                                                   \
    ASSERT_TRUE(files.HasMember("annotations.json"));                                              \
    ASSERT_TRUE(files["annotations.json"].HasMember("present annotations"));                       \
    {                                                                                              \
      bool has_annotation = false;                                                                 \
      for (const auto& annotation : files["annotations.json"]["present annotations"].GetArray()) { \
        if (strcmp(annotation.GetString(), name) == 0) {                                           \
          has_annotation = true;                                                                   \
          break;                                                                                   \
        }                                                                                          \
      }                                                                                            \
      EXPECT_TRUE(has_annotation&& name);                                                          \
    }                                                                                              \
  }

#define HAS_MISSING_ANNOTATION(json, name, error)                                            \
  {                                                                                          \
    ASSERT_TRUE(json.HasMember("files"));                                                    \
    auto& files = json["files"];                                                             \
    ASSERT_TRUE(files.HasMember("annotations.json"));                                        \
    ASSERT_TRUE(files["annotations.json"].HasMember("missing annotations"));                 \
    ASSERT_TRUE(files["annotations.json"]["missing annotations"].HasMember(name));           \
    EXPECT_STREQ(files["annotations.json"]["missing annotations"][name].GetString(), error); \
  }

#define HAS_COMPLETE_ATTACHMENT(json, name)                     \
  {                                                             \
    ASSERT_TRUE(json.HasMember("files"));                       \
    auto& files = json["files"];                                \
    ASSERT_TRUE(files.HasMember(name));                         \
    ASSERT_TRUE(files[name].HasMember("state"));                \
    EXPECT_STREQ(files[name]["state"].GetString(), "complete"); \
  }

#define HAS_PARTIAL_ATTACHMENT(json, name, error)              \
  {                                                            \
    ASSERT_TRUE(json.HasMember("files"));                      \
    auto& files = json["files"];                               \
    ASSERT_TRUE(files.HasMember(name));                        \
    ASSERT_TRUE(files[name].HasMember("state"));               \
    EXPECT_STREQ(files[name]["state"].GetString(), "partial"); \
    ASSERT_TRUE(files[name].HasMember("error"));               \
    EXPECT_STREQ(files[name]["error"].GetString(), error);     \
  }

#define HAS_MISSING_ATTACHMENT(json, name, error)              \
  {                                                            \
    ASSERT_TRUE(json.HasMember("files"));                      \
    auto& files = json["files"];                               \
    ASSERT_TRUE(files.HasMember(name));                        \
    ASSERT_TRUE(files[name].HasMember("state"));               \
    EXPECT_STREQ(files[name]["state"].GetString(), "missing"); \
    ASSERT_TRUE(files[name].HasMember("error"));               \
    EXPECT_STREQ(files[name]["error"].GetString(), error);     \
  }

namespace forensics {
namespace feedback_data {
namespace {

// Get the integrity metadata for the provided annotations and attachments, check that it adheres to
// the schema, and turn it into a json document
inline rapidjson::Document MakeJsonReport(const Metadata& metadata,
                                          const ::fit::result<Annotations>& annotations,
                                          const ::fit::result<Attachments>& attachments,
                                          const bool missing_non_platform_annotations = false) {
  const auto metadata_str =
      metadata.MakeMetadata(annotations, attachments, missing_non_platform_annotations);

  rapidjson::Document json;
  FX_CHECK(!json.Parse(metadata_str.c_str()).HasParseError());

  rapidjson::Document schema_json;
  FX_CHECK(!schema_json.Parse(kMetadataSchema).HasParseError());
  rapidjson::SchemaDocument schema(schema_json);
  rapidjson::SchemaValidator validator(schema);
  FX_CHECK(json.Accept(validator));

  // Convert to std::string to use its '==' operator.
  FX_CHECK(json["snapshot_version"].GetString() == std::string(SnapshotVersion::kString));
  FX_CHECK(json["metadata_version"].GetString() == std::string(Metadata::kVersion));

  return json;
}

TEST(MetadataTest, Check_AddsMissingAnnotationsOnNoAnnotations) {
  const AnnotationKeys annotation_allowlist = {
      "annotation 1",
  };

  const Metadata metadata(annotation_allowlist, /*attachment_allowlist=*/{});

  const auto metadata_json = MakeJsonReport(metadata, ::fit::error(), ::fit::error());
  HAS_MISSING_ANNOTATION(metadata_json, "annotation 1", "feedback logic error");
}

TEST(MetadataTest, Check_AddsMissingAnnotationsOnEmptyAnnotations) {
  const AnnotationKeys annotation_allowlist = {
      "annotation 1",
  };

  const Metadata metadata(annotation_allowlist, /*attachment_allowlist=*/{});

  const auto metadata_json = MakeJsonReport(metadata, ::fit::ok<Annotations>({}), ::fit::error());
  HAS_MISSING_ANNOTATION(metadata_json, "annotation 1", "feedback logic error");
}

TEST(MetadataTest, Check_AddsMissingAttachmentsOnNoAttachments) {
  const AttachmentKeys attachment_allowlist = {
      "attachment 1",
  };

  Metadata metadata(/*annotation_allowlist=*/{}, attachment_allowlist);

  const auto metadata_json = MakeJsonReport(metadata, ::fit::error(), ::fit::error());
  HAS_MISSING_ATTACHMENT(metadata_json, "attachment 1", "feedback logic error");
}

TEST(MetadataTest, Check_AddsMissingAttachmentsOnEmptyAttachments) {
  const AttachmentKeys attachment_allowlist = {
      "attachment 1",
  };

  Metadata metadata(/*annotation_allowlist=*/{}, attachment_allowlist);

  const auto metadata_json = MakeJsonReport(metadata, ::fit::error(), ::fit::ok<Attachments>({}));
  HAS_MISSING_ATTACHMENT(metadata_json, "attachment 1", "feedback logic error");
}

TEST(MetadataTest, Check_FormatAnnotationsProperly) {
  const AnnotationKeys annotation_allowlist = {
      "present annotation 1",
      "present annotation 2",
      "missing annotation 1",
      "missing annotation 2",
  };

  const Annotations annotations = {
      {"present annotation 1", AnnotationOr("")},
      {"present annotation 2", AnnotationOr("")},
      {"missing annotation 1", AnnotationOr(Error::kConnectionError)},
      {"missing annotation 2", AnnotationOr(Error::kFileWriteFailure)},
  };

  Metadata metadata(annotation_allowlist, /*attachment_allowlist=*/{});

  const auto metadata_json =
      MakeJsonReport(metadata, ::fit::ok(std::move(annotations)), ::fit::error());

  ANNOTATIONS_JSON_STATE_IS(metadata_json, "partial");

  HAS_PRESENT_ANNOTATION(metadata_json, "present annotation 1");
  HAS_PRESENT_ANNOTATION(metadata_json, "present annotation 2");

  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 1", "FIDL connection error");
  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 2", "file write failure");
}

TEST(MetadataTest, Check_FormatAttachmentsProperly) {
  const AttachmentKeys attachment_allowlist = {
      "complete attachment 1", "complete attachment 2", "partial attachment 1",
      "partial attachment 2",  "missing attachment 1",  "missing attachment 2",
  };

  const Attachments attachments = {
      {"complete attachment 1", AttachmentValue("")},
      {"complete attachment 2", AttachmentValue("")},
      {"partial attachment 1", AttachmentValue("", Error::kTimeout)},
      {"partial attachment 2", AttachmentValue("", Error::kAsyncTaskPostFailure)},
      {"missing attachment 1", AttachmentValue(Error::kBadValue)},
      {"missing attachment 2", AttachmentValue(Error::kFileReadFailure)},
  };

  Metadata metadata(/*annotation_allowlist=*/{}, attachment_allowlist);

  const auto metadata_json =
      MakeJsonReport(metadata, ::fit::error(), ::fit::ok<Attachments>(std::move(attachments)));

  HAS_COMPLETE_ATTACHMENT(metadata_json, "complete attachment 1");
  HAS_COMPLETE_ATTACHMENT(metadata_json, "complete attachment 2");

  HAS_PARTIAL_ATTACHMENT(metadata_json, "partial attachment 1", "data collection timeout");
  HAS_PARTIAL_ATTACHMENT(metadata_json, "partial attachment 2", "async post task failure");

  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 1", "bad data returned");
  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 2", "file read failure");
}

TEST(MetadataTest, Check_NonPlatformAnnotationsComplete) {
  const Annotations annotations = {
      {"non-platform annotation", AnnotationOr("")},
  };

  Metadata metadata(/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{});

  const auto metadata_json =
      MakeJsonReport(metadata, ::fit::ok(std::move(annotations)), ::fit::error());

  HAS_PRESENT_ANNOTATION(metadata_json, "non-platform annotations");
}

TEST(MetadataTest, Check_NonPlatformAnnotationsPartial) {
  const Annotations annotations = {
      {"non-platform annotation", AnnotationOr("")},
  };

  Metadata metadata(/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{});

  const auto metadata_json =
      MakeJsonReport(metadata, ::fit::ok(std::move(annotations)), ::fit::error(),
                     /*missing_non_platform_annotations=*/true);

  HAS_MISSING_ANNOTATION(metadata_json, "non-platform annotations",
                         "too many non-platfrom annotations added");
}

TEST(MetadataTest, Check_NonPlatformAnnotationsMissing) {
  Metadata metadata(/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{});

  const auto metadata_json = MakeJsonReport(metadata, ::fit::error(), ::fit::error(),
                                            /*missing_non_platform_annotations=*/true);

  HAS_MISSING_ANNOTATION(metadata_json, "non-platform annotations",
                         "too many non-platfrom annotations added");
}

TEST(MetadataTest, Check_SmokeTest) {
  const AnnotationKeys annotation_allowlist = {
      "present annotation 1", "present annotation 2", "missing annotation 1",
      "missing annotation 2", "missing annotation 3",
  };

  const Annotations annotations = {
      {"present annotation 1", AnnotationOr("")},
      {"present annotation 2", AnnotationOr("")},
      {"missing annotation 1", AnnotationOr(Error::kConnectionError)},
      {"missing annotation 2", AnnotationOr(Error::kFileWriteFailure)},
      {"non-platform annotation 1", AnnotationOr("")},
  };

  const AttachmentKeys attachment_allowlist = {
      "complete attachment 1", "complete attachment 2", "partial attachment 1",
      "partial attachment 2",  "missing attachment 1",  "missing attachment 2",
      "missing attachment 3",
  };
  const Attachments attachments = {
      {"complete attachment 1", AttachmentValue("")},
      {"complete attachment 2", AttachmentValue("")},
      {"partial attachment 1", AttachmentValue("", Error::kTimeout)},
      {"partial attachment 2", AttachmentValue("", Error::kAsyncTaskPostFailure)},
      {"missing attachment 1", AttachmentValue(Error::kBadValue)},
      {"missing attachment 2", AttachmentValue(Error::kFileReadFailure)},
  };

  Metadata metadata(annotation_allowlist, attachment_allowlist);

  const auto metadata_json =
      MakeJsonReport(metadata, ::fit::ok<Annotations>(std::move(annotations)),
                     ::fit::ok<Attachments>(std::move(attachments)),
                     /*missing_non_platform_annotations=*/true);

  HAS_COMPLETE_ATTACHMENT(metadata_json, "complete attachment 1");
  HAS_COMPLETE_ATTACHMENT(metadata_json, "complete attachment 2");

  HAS_PARTIAL_ATTACHMENT(metadata_json, "partial attachment 1", "data collection timeout");
  HAS_PARTIAL_ATTACHMENT(metadata_json, "partial attachment 2", "async post task failure");

  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 1", "bad data returned");
  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 2", "file read failure");
  HAS_MISSING_ATTACHMENT(metadata_json, "missing attachment 3", "feedback logic error");

  ANNOTATIONS_JSON_STATE_IS(metadata_json, "partial");

  HAS_PRESENT_ANNOTATION(metadata_json, "present annotation 1");
  HAS_PRESENT_ANNOTATION(metadata_json, "present annotation 2");

  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 1", "FIDL connection error");
  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 2", "file write failure");
  HAS_MISSING_ANNOTATION(metadata_json, "missing annotation 3", "feedback logic error");

  HAS_MISSING_ANNOTATION(metadata_json, "non-platform annotations",
                         "too many non-platfrom annotations added");
}

TEST(MetadataTest, Check_EmptySnapshot) {
  Metadata metadata(/*annotation_allowlist=*/{}, /*attachment_allowlist=*/{});

  auto metadata_str = metadata.MakeMetadata(::fit::error(), ::fit::error(),
                                            /*missing_non_platform_annotations=*/false);

  rapidjson::Document json;
  ASSERT_TRUE(!json.Parse(metadata_str.c_str()).HasParseError());

  rapidjson::Document schema_json;
  ASSERT_TRUE(!schema_json.Parse(kMetadataSchema).HasParseError());
  rapidjson::SchemaDocument schema(schema_json);
  rapidjson::SchemaValidator validator(schema);
  ASSERT_TRUE(json.Accept(validator));

  // Convert to std::string to use its '==' operator.
  EXPECT_STREQ(json["snapshot_version"].GetString(), SnapshotVersion::kString);
  EXPECT_STREQ(json["metadata_version"].GetString(), Metadata::kVersion);

  EXPECT_TRUE(json.HasMember("files"));
  EXPECT_TRUE(json["files"].IsObject());
  EXPECT_TRUE(json["files"].GetObject().ObjectEmpty());
}

struct TestParam {
  std::string test_name;
  AnnotationKeys annotation_allowlist;
  Annotations annotations;
  bool missing_non_platform_annotations;
  std::string state;
};

class AnnotationsJsonStateTest : public testing::Test,
                                 public testing::WithParamInterface<TestParam> {};

INSTANTIATE_TEST_SUITE_P(WithVariousAnnotations, AnnotationsJsonStateTest,
                         ::testing::ValuesIn(
                             std::vector<TestParam>(
                                 {
                                     TestParam{
                                         .test_name = "CompletePlatform_CompleteNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"platform", AnnotationOr("")},
                                                 {"non-platform", AnnotationOr("")},
                                             },
                                         .missing_non_platform_annotations = false,
                                         .state = "complete",

                                     },
                                     TestParam{
                                         .test_name = "CompletePlatform_PartialNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"platform", AnnotationOr("")},
                                                 {"non-platform", AnnotationOr("")},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "CompletePlatform_MissingNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"platform", AnnotationOr("")},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "PartialPlatform_CompleteNonPlatform",
                                         .annotation_allowlist = {"platform 1", "platform 2"},
                                         .annotations =
                                             {
                                                 {"platform 1", AnnotationOr("")},
                                                 {"non-platform", AnnotationOr("")},
                                             },
                                         .missing_non_platform_annotations = false,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "PartialPlatform_PartialNonPlatform",
                                         .annotation_allowlist = {"platform 1", "platform 2"},
                                         .annotations =
                                             {
                                                 {"platform 1", AnnotationOr("")},
                                                 {"non-platform", AnnotationOr("")},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "PartialPlatform_MissingNonPlatform",
                                         .annotation_allowlist = {"platform 1", "platform 2"},
                                         .annotations =
                                             {
                                                 {"platform 1", AnnotationOr("")},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "MissingPlatform_CompleteNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"non-platform", AnnotationOr("")},
                                             },
                                         .missing_non_platform_annotations = false,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "MissingPlatform_PartialNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations =
                                             {
                                                 {"non-platform", AnnotationOr("")},
                                             },
                                         .missing_non_platform_annotations = true,
                                         .state = "partial",

                                     },
                                     TestParam{
                                         .test_name = "MissingPlatform_MissingNonPlatform",
                                         .annotation_allowlist = {"platform"},
                                         .annotations = {},
                                         .missing_non_platform_annotations = true,
                                         .state = "missing",

                                     },
                                 })),
                         [](const testing::TestParamInfo<TestParam>& info) {
                           return info.param.test_name;
                         });
TEST_P(AnnotationsJsonStateTest, Succeed) {
  const auto param = GetParam();
  const Metadata metadata(param.annotation_allowlist, /*attachment_allowlist=*/{});

  const auto metadata_json = MakeJsonReport(metadata, ::fit::ok(param.annotations), ::fit::error(),
                                            param.missing_non_platform_annotations);
  ANNOTATIONS_JSON_STATE_IS(metadata_json, param.state.c_str());
}

}  // namespace
}  // namespace feedback_data
}  // namespace forensics