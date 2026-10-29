// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ArcadeControlsSettingsWidget.h"
#include "SettingsWindow.h"

#include "pcsx2/Host.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QFont>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace
{
	using GIB = GenericInputBinding;

	// ArcadeInput hands out names untranslated, in the "Arcade" context.
	QString Translated(const char* text)
	{
		return QString::fromStdString(Host::TranslateToString("Arcade", text));
	}

	bool JobHasButton(const ArcadeInput::Job& job, GIB button)
	{
		return std::find(job.buttons.begin(), job.buttons.end(), button) != job.buttons.end();
	}
} // namespace

ArcadeControlsSettingsWidget::ArcadeControlsSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent)
	: SettingsWidget(settings_dialog, parent)
	, m_per_game(settings_dialog->isPerGameSettings())
	, m_gameid(settings_dialog->getSerial())
{
	if (m_per_game)
		m_layout = ArcadeInput::GetLayout(m_gameid);

	buildUi();
	loadRemaps();
	refreshRows();
}

ArcadeControlsSettingsWidget::~ArcadeControlsSettingsWidget() = default;

void ArcadeControlsSettingsWidget::buildUi()
{
	QWidget* contents = new QWidget(this);
	QVBoxLayout* layout = new QVBoxLayout(contents);

	if (m_per_game)
	{
		const ArcadeInput::ModeInfo& mode = ArcadeInput::GetModeInfo(m_layout.mode);
		m_mode_label = new QLabel(contents);
		m_mode_label->setObjectName(QStringLiteral("arcadeModeLabel"));
		m_mode_label->setWordWrap(true);
		m_mode_label->setTextFormat(Qt::RichText);
		m_mode_label->setText(tr("Cabinet type: <b>%1</b>. %2").arg(Translated(mode.display_name).toHtmlEscaped(), Translated(mode.hint).toHtmlEscaped()));
		layout->addWidget(m_mode_label);

		QLabel* intro = new QLabel(
			tr("Choose what each button of your controller does on this game's cabinet. The buttons are the ones you set up in "
			   "Settings > Controllers. A button you change here is shown in bold. Buttons you leave alone follow the layout for "
			   "all arcade games (Settings > Arcade Controls), which in turn starts from the cabinet's own. Changes apply at once, "
			   "also while the game runs."),
			contents);
		intro->setWordWrap(true);
		layout->addWidget(intro);
	}
	else
	{
		QLabel* intro = new QLabel(
			tr("The layout every arcade game starts from. Each controller button acts as the button chosen here: leave a button "
			   "as itself to keep the cabinet's own layout, pick another button to give it that button's job, or Nothing to "
			   "turn it off. A game's own settings (Properties > Arcade Controls) can change this for that game alone. Changes "
			   "apply at once, also while a game runs."),
			contents);
		intro->setWordWrap(true);
		layout->addWidget(intro);
	}

	QGridLayout* grid = new QGridLayout();
	grid->setColumnStretch(1, 1);
	const auto header = [contents](const QString& text) {
		QLabel* label = new QLabel(text, contents);
		QFont font = label->font();
		font.setBold(true);
		label->setFont(font);
		return label;
	};
	grid->addWidget(header(tr("Controller button")), 0, 0);
	grid->addWidget(header(m_per_game ? tr("Does on this cabinet") : tr("Acts as")), 0, 1);
	grid->addWidget(header(m_per_game ? tr("Set by") : QString()), 0, 2);

	const std::span<const ArcadeInput::ButtonInfo> buttons = ArcadeInput::GetRemappableButtons();
	for (const ArcadeInput::ButtonInfo& button : buttons)
	{
		Row row;
		row.button = &button;
		row.name = new QLabel(Translated(button.display_name), contents);
		row.combo = new QComboBox(contents);
		row.combo->setObjectName(QStringLiteral("arcadeButton_%1").arg(QLatin1String(button.name)));
		row.source = new QLabel(contents);

		if (m_per_game)
		{
			const auto fixed_job = std::find_if(m_layout.jobs.begin(), m_layout.jobs.end(),
				[&button](const ArcadeInput::Job& job) { return job.fixed && JobHasButton(job, button.binding); });
			if (fixed_job != m_layout.jobs.end())
			{
				// A driving cabinet's pedals: the triggers stay the pedals.
				row.fixed = true;
				row.combo->addItem(tr("%1 (analog)").arg(QString::fromStdString(fixed_job->label)), -1);
				row.combo->setEnabled(false);
			}
			else
			{
				for (size_t i = 0; i < m_layout.jobs.size(); i++)
				{
					if (!m_layout.jobs[i].fixed)
						row.combo->addItem(QString::fromStdString(m_layout.jobs[i].label), static_cast<int>(i));
				}
				row.combo->addItem(tr("Nothing"), -1);
			}
		}
		else
		{
			for (const ArcadeInput::ButtonInfo& target : buttons)
				row.combo->addItem(Translated(target.display_name), static_cast<int>(target.binding));
			row.combo->addItem(tr("Nothing"), static_cast<int>(GIB::Unknown));
		}

		const int grid_row = static_cast<int>(m_rows.size()) + 1;
		grid->addWidget(row.name, grid_row, 0);
		grid->addWidget(row.combo, grid_row, 1);
		grid->addWidget(row.source, grid_row, 2);
		m_rows.push_back(row);
	}
	layout->addLayout(grid);

	QHBoxLayout* buttons_row = new QHBoxLayout();
	m_reset_button = new QPushButton(m_per_game ? tr("Reset to the Layout for All Arcade Games") : tr("Reset to the Cabinet's Layout"), contents);
	m_reset_button->setObjectName(QStringLiteral("arcadeResetButton"));
	buttons_row->addWidget(m_reset_button);
	buttons_row->addStretch(1);
	layout->addLayout(buttons_row);
	layout->addStretch(1);

	addTab(QString(), contents);

	// Wired after the rows exist, and by index: the combos are made once.
	for (size_t i = 0; i < m_rows.size(); i++)
	{
		connect(m_rows[i].combo, QOverload<int>::of(&QComboBox::activated), this, [this, i](int index) { onChoice(i, index); });
	}
	connect(m_reset_button, &QPushButton::clicked, this, &ArcadeControlsSettingsWidget::onResetClicked);
}

void ArcadeControlsSettingsWidget::loadRemaps()
{
	m_global = ArcadeInput::RemapFromString(Host::GetBaseStringSettingValue(ArcadeInput::REMAP_SECTION, ArcadeInput::REMAP_KEY, ""));
	if (m_per_game)
	{
		m_game = ArcadeInput::RemapFromString(
			dialog()->getStringValue(ArcadeInput::REMAP_SECTION, ArcadeInput::REMAP_KEY, "").value_or(std::string()));
	}
}

void ArcadeControlsSettingsWidget::showEvent(QShowEvent* event)
{
	SettingsWidget::showEvent(event);

	// The other settings window may have changed the global layout since this one was built.
	loadRemaps();
	refreshRows();
}

void ArcadeControlsSettingsWidget::refreshRows()
{
	for (Row& row : m_rows)
	{
		const GIB button = row.button->binding;
		const QSignalBlocker blocker(row.combo);
		QString source;
		bool changed = false;

		if (m_per_game)
		{
			if (row.fixed)
			{
				row.combo->setCurrentIndex(0);
			}
			else
			{
				const ArcadeInput::Job* job = ArcadeInput::GetJobOf(m_layout, m_global, m_game, button);
				const int job_index = job ? static_cast<int>(job - m_layout.jobs.data()) : -1;
				row.combo->setCurrentIndex(row.combo->findData(job_index));
			}

			if (m_game.count(button))
			{
				source = tr("This game");
				changed = true;
			}
			else if (m_global.count(button))
			{
				source = tr("All arcade games");
			}
		}
		else
		{
			const auto it = m_global.find(button);
			const GIB target = (it != m_global.end()) ? it->second : button;
			row.combo->setCurrentIndex(row.combo->findData(static_cast<int>(target)));
			changed = (it != m_global.end());
		}

		row.source->setText(source);
		QFont font = row.name->font();
		font.setBold(changed);
		row.name->setFont(font);
	}

	m_reset_button->setEnabled(m_per_game ? !m_game.empty() : !m_global.empty());
}

void ArcadeControlsSettingsWidget::onChoice(size_t row_index, int combo_index)
{
	Row& row = m_rows[row_index];
	const int data = row.combo->itemData(combo_index).toInt();

	if (m_per_game)
	{
		const ArcadeInput::Job* job = (data >= 0) ? &m_layout.jobs[static_cast<size_t>(data)] : nullptr;
		ArcadeInput::SetJob(m_game, m_global, m_layout, row.button->binding, job);
		saveRemap(m_game);
	}
	else
	{
		ArcadeInput::SetGlobalTarget(m_global, row.button->binding, static_cast<GIB>(data));
		saveRemap(m_global);
	}

	refreshRows();
}

void ArcadeControlsSettingsWidget::onResetClicked()
{
	if (!m_per_game &&
		QMessageBox::question(this, tr("Arcade Controls"),
			tr("Put every button back to the cabinet's own layout in all arcade games?")) != QMessageBox::Yes)
	{
		return;
	}

	ArcadeInput::Remap& remap = m_per_game ? m_game : m_global;
	remap.clear();
	saveRemap(remap);
	refreshRows();
}

void ArcadeControlsSettingsWidget::saveRemap(const ArcadeInput::Remap& remap)
{
	// Goes through the window so it lands in the game's file or the global settings like every other
	// page, and a running game picks it up.
	const std::string text = ArcadeInput::RemapToString(remap);
	if (text.empty())
		dialog()->setStringSettingValue(ArcadeInput::REMAP_SECTION, ArcadeInput::REMAP_KEY, std::nullopt);
	else
		dialog()->setStringSettingValue(ArcadeInput::REMAP_SECTION, ArcadeInput::REMAP_KEY, text.c_str());
}

#include "moc_ArcadeControlsSettingsWidget.cpp"
