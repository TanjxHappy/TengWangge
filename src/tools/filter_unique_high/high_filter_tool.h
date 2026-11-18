#pragma once

#include "tools/filter_unique_high/high_filter_page.h"

#include <QPointer>

namespace tw {

class HighFilterTool : public ITool
{
public:
    ~HighFilterTool() override;

    ToolInfo info() const override;
    bool initialize(const ToolContext &context, QString *error) override;
    QVector<ToolPage> createPages() override;
    void shutdown() override;

    HighFilterPage *page() const { return m_page; }

private:
    ToolContext m_context;
    QString m_dataDir;
    QPointer<HighFilterPage> m_page;
};

} // namespace tw
